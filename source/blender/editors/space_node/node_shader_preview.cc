/* SPDX-FileCopyrightText: 2023 Blender Foundation
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edrend
 *
 * This file implements shader node previews which rely on a structure owned by each SpaceNode.
 * We take advantage of the RenderResult available as ImBuf images to store a Render for every
 * viewed nested node tree present in a SpaceNode. The computation is initiated at the moment of
 * drawing nodes overlays. One render is started for the current nodetree, having a ViewLayer
 * associated with each previewed node.
 *
 * We separate the previewed nodes in two categories: the shader ones and the non-shader ones.
 * - for non-shader nodes, we use AOVs(Arbitrary Output Variable) which highly speed up the
 * rendering process by rendering every non-shader node at the same time. They are rendered in the
 * first ViewLayer.
 * - for shader nodes, we render them each in a different ViewLayer, by routing the node to the
 * output of the material in the preview scene.
 *
 * At the moment of drawing, we take the Render of the viewed node tree and extract the ImBuf of
 * the wanted viewlayer/pass for each previewed node.
 */

#include "DNA_camera_types.h"
#include "DNA_material_types.h"
#include "DNA_world_types.h"

#include "BKE_colortools.h"
#include "BKE_compute_contexts.hh"
#include "BKE_context.h"
#include "BKE_global.h"
#include "BKE_layer.h"
#include "BKE_lib_id.h"
#include "BKE_main.h"
#include "BKE_material.h"
#include "BKE_node.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_tree_update.h"
#include "BKE_scene.h"

#include "DEG_depsgraph.h"

#include "IMB_imbuf.h"

#include "WM_api.hh"

#include "ED_datafiles.h"
#include "ED_node_preview.hh"
#include "ED_render.hh"
#include "ED_screen.hh"
#include "node_intern.hh"

namespace blender::ed::space_node {
/* -------------------------------------------------------------------- */
/** \name Local Structs
 * \{ */
using NodeSocketPair = std::pair<bNode *, bNodeSocket *>;

struct ShaderNodesPreviewJob {
  NestedTreePreviews *tree_previews;
  Scene *scene;
  /* Pointer to the job's stop variable which is used to know when the job is asked for finishing.
   * The idea is that the renderer will read this value frequently and abort the render if it is
   * true. */
  bool *stop;
  /* Pointer to the job's update variable which is set to true to refresh the UI when the renderer
   * is delivering a fresh result. It allows the job to give some UI refresh tags to the WM. */
  bool *do_update;

  Material *mat_copy;
  bNode *mat_output_copy;
  NodeSocketPair mat_displacement_copy;
  /* TreePath used to locate the nodetree.
   * bNodeTreePath elements have some listbase pointers which should not be used. */
  Vector<bNodeTreePath *> treepath_copy;
  Vector<bNode *> AOV_nodes;
  Vector<bNode *> shader_nodes;

  bNode *rendering_node;
  bool rendering_AOVs;

  Main *bmain;
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Compute Context functions
 * \{ */

static void ensure_nodetree_previews(const bContext &C,
                                     NestedTreePreviews &tree_previews,
                                     Material &material,
                                     ListBase &treepath);

static std::optional<ComputeContextHash> get_compute_context_hash_for_node_editor(
    const SpaceNode &snode)
{
  Vector<const bNodeTreePath *> treepath = snode.treepath;
  if (treepath.is_empty()) {
    return std::nullopt;
  }
  if (treepath.size() == 1) {
    /* Top group. */
    ComputeContextHash hash;
    hash.v1 = hash.v2 = 0;
    return hash;
  }
  ComputeContextBuilder compute_context_builder;
  for (const int i : treepath.index_range().drop_back(1)) {
    /* The tree path contains the name of the node but not its ID. */
    const bNode *node = nodeFindNodebyName(treepath[i]->nodetree, treepath[i + 1]->node_name);
    if (node == nullptr) {
      /* The current tree path is invalid, probably because some parent group node has been
       * deleted. */
      return std::nullopt;
    }
    compute_context_builder.push<bke::NodeGroupComputeContext>(*node);
  }
  return compute_context_builder.hash();
}

/*
 * This function returns the `NestedTreePreviews *` for the nodetree shown in the SpaceNode.
 * This is the first function in charge of the previews by calling `ensure_nodetree_previews`.
 */
NestedTreePreviews *get_nested_previews(const bContext &C, SpaceNode &snode)
{
  if (snode.id == nullptr || GS(snode.id->name) != ID_MA) {
    return nullptr;
  }
  NestedTreePreviews *tree_previews = nullptr;
  if (auto hash = get_compute_context_hash_for_node_editor(snode)) {
    tree_previews = snode.runtime->tree_previews_per_context
                        .lookup_or_add_cb(*hash,
                                          [&]() {
                                            return std::make_unique<NestedTreePreviews>(
                                                U.node_preview_res);
                                          })
                        .get();
    Material *ma = reinterpret_cast<Material *>(snode.id);
    ensure_nodetree_previews(C, *tree_previews, *ma, snode.treepath);
  }
  return tree_previews;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Preview scene
 * \{ */

static Material *duplicate_material(const Material &mat)
{
  Material *ma_copy = reinterpret_cast<Material *>(
      BKE_id_copy_ex(nullptr,
                     &mat.id,
                     nullptr,
                     LIB_ID_CREATE_LOCAL | LIB_ID_COPY_LOCALIZE | LIB_ID_COPY_NO_ANIMDATA));
  return ma_copy;
}

static Scene *preview_prepare_scene(const Main *bmain,
                                    const Scene *scene_orig,
                                    Main *pr_main,
                                    Material *mat_copy)
{
  Scene *scene_preview;

  memcpy(pr_main->filepath, BKE_main_blendfile_path(bmain), sizeof(pr_main->filepath));

  if (pr_main == nullptr) {
    return nullptr;
  }
  scene_preview = static_cast<Scene *>(pr_main->scenes.first);
  if (scene_preview == nullptr) {
    return nullptr;
  }

  ViewLayer *view_layer = static_cast<ViewLayer *>(scene_preview->view_layers.first);

  /* Only enable the combined render-pass. */
  view_layer->passflag = SCE_PASS_COMBINED;
  view_layer->eevee.render_passes = 0;

  /* This flag tells render to not execute depsgraph or F-Curves etc. */
  scene_preview->r.scemode |= R_BUTS_PREVIEW;
  scene_preview->r.mode |= R_PERSISTENT_DATA;
  STRNCPY(scene_preview->r.engine, scene_orig->r.engine);

  scene_preview->r.color_mgt_flag = scene_orig->r.color_mgt_flag;
  BKE_color_managed_display_settings_copy(&scene_preview->display_settings,
                                          &scene_orig->display_settings);

  BKE_color_managed_view_settings_free(&scene_preview->view_settings);
  BKE_color_managed_view_settings_copy(&scene_preview->view_settings, &scene_orig->view_settings);

  scene_preview->r.alphamode = R_ADDSKY;

  scene_preview->r.cfra = scene_orig->r.cfra;

  /* Setup the world. */
  scene_preview->world = ED_preview_prepare_world(
      pr_main, scene_preview, scene_orig->world, ID_MA, PR_BUTS_RENDER);

  BLI_addtail(&pr_main->materials, mat_copy);
  scene_preview->world->use_nodes = false;
  scene_preview->world->horr = 0.05f;
  scene_preview->world->horg = 0.05f;
  scene_preview->world->horb = 0.05f;

  ED_preview_set_visibility(
      pr_main, scene_preview, view_layer, ePreviewType(mat_copy->pr_type), PR_BUTS_RENDER);

  BKE_view_layer_synced_ensure(scene_preview, view_layer);
  LISTBASE_FOREACH (Base *, base, BKE_view_layer_object_bases_get(view_layer)) {
    if (base->object->id.name[2] == 'p') {
      if (OB_TYPE_SUPPORT_MATERIAL(base->object->type)) {
        /* Don't use BKE_object_material_assign, it changed mat->id.us, which shows in the UI. */
        Material ***matar = BKE_object_material_array_p(base->object);
        int actcol = max_ii(base->object->actcol - 1, 0);

        if (matar && actcol < base->object->totcol) {
          (*matar)[actcol] = mat_copy;
        }
      }
      else if (base->object->type == OB_LAMP) {
        base->flag |= BASE_ENABLED_AND_MAYBE_VISIBLE_IN_VIEWPORT;
      }
    }
  }

  return scene_preview;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Preview rendering
 * \{ */

/* Return the socket used for previewing the node (should probably follow more precise rules). */
static const bNodeSocket *node_find_preview_socket(const bNode &node)
{
  LISTBASE_FOREACH (bNodeSocket *, socket, &node.outputs) {
    if (socket->is_visible()) {
      return socket;
    }
  }
  return nullptr;
}

static bool node_use_aov(const bNode &node)
{
  const bNodeSocket *socket = node_find_preview_socket(node);
  return socket != nullptr && socket->type != SOCK_SHADER;
}

static ImBuf *get_image_from_viewlayer_and_pass(RenderResult &rr,
                                                const char *layer_name,
                                                const char *pass_name)
{
  RenderLayer *rl;
  if (layer_name) {
    rl = RE_GetRenderLayer(&rr, layer_name);
  }
  else {
    rl = static_cast<RenderLayer *>(rr.layers.first);
  }
  if (rl == nullptr) {
    return nullptr;
  }
  RenderPass *rp;
  if (pass_name) {
    rp = RE_pass_find_by_name(rl, pass_name, nullptr);
  }
  else {
    rp = static_cast<RenderPass *>(rl->passes.first);
  }
  ImBuf *ibuf = rp ? rp->ibuf : nullptr;
  return ibuf;
}

/* `node_release_preview_ibuf` should be called after this. */
ImBuf *node_preview_acquire_ibuf(bNodeTree &ntree,
                                 NestedTreePreviews &tree_previews,
                                 const bNode &node)
{
  if (tree_previews.previews_render == nullptr) {
    return nullptr;
  }

  RenderResult *rr = RE_AcquireResultRead(tree_previews.previews_render);
  ImBuf *&image_cached = tree_previews.previews_map.lookup_or_add(node.identifier, nullptr);
  if (rr == nullptr) {
    return image_cached;
  }
  if (image_cached == nullptr) {
    if (tree_previews.rendering == false) {
      ntree.runtime->previews_refresh_state++;
    }
    else {
      /* When the render process is started, the user must see that the preview area is open. */
      ImBuf *image_latest = nullptr;
      if (node_use_aov(node)) {
        image_latest = get_image_from_viewlayer_and_pass(*rr, nullptr, node.name);
      }
      else {
        image_latest = get_image_from_viewlayer_and_pass(*rr, node.name, nullptr);
      }
      if (image_latest) {
        IMB_refImBuf(image_latest);
        image_cached = image_latest;
      }
    }
  }
  return image_cached;
}

void node_release_preview_ibuf(NestedTreePreviews &tree_previews)
{
  if (tree_previews.previews_render == nullptr) {
    return;
  }
  RE_ReleaseResult(tree_previews.previews_render);
}

/**
 * Get a link to the node outside the nested node-groups by creating a new output socket for each
 * nested node-group. To do so we cover all nested node-trees starting from the farthest, and
 * update the `nested_node_iter` pointer to the current node-group instance used for linking.
 * We stop before getting to the main node-tree because the output type is different.
 */
static void connect_nested_node_to_node(const Span<bNodeTreePath *> treepath,
                                        bNode &nested_node,
                                        bNodeSocket &nested_socket,
                                        bNode &final_node,
                                        bNodeSocket &final_socket)
{
  bNode *nested_node_iter = &nested_node;
  bNodeSocket *nested_socket_iter = &nested_socket;
  for (int i = treepath.size() - 1; i > 0; --i) {
    bNodeTreePath *path = treepath[i];
    bNodeTreePath *path_prev = treepath[i - 1];
    bNodeTree *nested_nt = path->nodetree;
    bNode *output_node = nullptr;
    for (bNode *iter_node : nested_nt->all_nodes()) {
      if (iter_node->is_group_output() && iter_node->flag & NODE_DO_OUTPUT) {
        output_node = iter_node;
        break;
      }
    }
    if (output_node == nullptr) {
      output_node = nodeAddStaticNode(nullptr, nested_nt, NODE_GROUP_OUTPUT);
      output_node->flag |= NODE_DO_OUTPUT;
    }

    ntreeAddSocketInterface(nested_nt, SOCK_OUT, nested_socket_iter->idname, nested_node.name);
    BKE_ntree_update_main_tree(G.pr_main, nested_nt, nullptr);
    bNodeSocket *out_socket = bke::node_find_enabled_input_socket(*output_node, nested_node.name);

    nodeAddLink(nested_nt, nested_node_iter, nested_socket_iter, output_node, out_socket);
    BKE_ntree_update_main_tree(G.pr_main, nested_nt, nullptr);

    /* Change the `nested_node` pointer to the nested node-group instance node. The tree path
     * contains the name of the instance node but not its ID. */
    nested_node_iter = nodeFindNodebyName(path_prev->nodetree, path->node_name);

    /* Update the sockets of the node because we added a new interface. */
    BKE_ntree_update_tag_node_property(path_prev->nodetree, nested_node_iter);
    BKE_ntree_update_main_tree(G.pr_main, path_prev->nodetree, nullptr);

    /* Now use the newly created socket of the node-group as previewing socket of the node-group
     * instance node. */
    nested_socket_iter = bke::node_find_enabled_output_socket(*nested_node_iter, nested_node.name);
  }

  nodeAddLink(treepath.first()->nodetree,
              nested_node_iter,
              nested_socket_iter,
              &final_node,
              &final_socket);
}

/* Connect the node to the output of the first nodetree from `treepath`. Last element of `treepath`
 * should be the path to the node's nodetree */
static void connect_node_to_surface_output(const Span<bNodeTreePath *> treepath,
                                           bNode &node,
                                           bNode &output_node)
{
  bNodeSocket *out_surface_socket = nullptr;
  bNodeTree *main_nt = treepath.first()->nodetree;
  bNodeSocket *node_preview_socket = const_cast<bNodeSocket *>(node_find_preview_socket(node));
  if (node_preview_socket == nullptr) {
    return;
  }
  /* Ensure output is usable. */
  out_surface_socket = nodeFindSocket(&output_node, SOCK_IN, "Surface");
  if (out_surface_socket->link) {
    /* Make sure no node is already wired to the output before wiring. */
    nodeRemLink(main_nt, out_surface_socket->link);
  }

  connect_nested_node_to_node(
      treepath, node, *node_preview_socket, output_node, *out_surface_socket);
  BKE_ntree_update_main_tree(G.pr_main, main_nt, nullptr);
}

/* Connect the nodes to some aov nodes located in the first nodetree from `treepath`. Last element
 * of `treepath` should be the path to the nodes nodetree. */
static void connect_nodes_to_aovs(const Span<bNodeTreePath *> treepath, const Span<bNode *> &nodes)
{
  if (nodes.size() == 0) {
    return;
  }
  bNodeTree *main_nt = treepath.first()->nodetree;
  for (bNode *node : nodes) {
    bNodeSocket *node_preview_socket = const_cast<bNodeSocket *>(node_find_preview_socket(*node));

    bNode *aov_node = nodeAddStaticNode(nullptr, main_nt, SH_NODE_OUTPUT_AOV);
    strcpy(reinterpret_cast<NodeShaderOutputAOV *>(aov_node->storage)->name, node->name);
    if (node_preview_socket == nullptr) {
      continue;
    }
    bNodeSocket *aov_socket = nodeFindSocket(aov_node, SOCK_IN, "Color");

    connect_nested_node_to_node(treepath, *node, *node_preview_socket, *aov_node, *aov_socket);
  }
  BKE_ntree_update_main_tree(G.pr_main, main_nt, nullptr);
}

/* Called by renderer, checks job stops. */
static bool nodetree_previews_break(void *spv)
{
  ShaderNodesPreviewJob *job_data = static_cast<ShaderNodesPreviewJob *>(spv);

  return *(job_data->stop);
}

static bool prepare_viewlayer_update(void *pvl_data, ViewLayer *vl, Depsgraph *depsgraph)
{
  bNode *node = nullptr;
  ShaderNodesPreviewJob *job_data = static_cast<ShaderNodesPreviewJob *>(pvl_data);
  for (bNode *node_iter : job_data->shader_nodes) {
    if (STREQ(vl->name, node_iter->name)) {
      node = node_iter;
      job_data->rendering_node = node_iter;
      job_data->rendering_AOVs = false;
      break;
    }
  }
  if (node == nullptr) {
    job_data->rendering_node = nullptr;
    job_data->rendering_AOVs = true;
    /* The AOV layer is the default `ViewLayer` of the scene(which should be the first one). */
    return job_data->AOV_nodes.size() > 0 && !vl->prev;
  }

  bNodeSocket *displacement_socket = nodeFindSocket(
      job_data->mat_output_copy, SOCK_IN, "Displacement");
  if (job_data->mat_displacement_copy.first != nullptr && displacement_socket->link == nullptr) {
    nodeAddLink(job_data->treepath_copy.first()->nodetree,
                job_data->mat_displacement_copy.first,
                job_data->mat_displacement_copy.second,
                job_data->mat_output_copy,
                displacement_socket);
  }
  connect_node_to_surface_output(job_data->treepath_copy, *node, *job_data->mat_output_copy);

  if (depsgraph != nullptr) {
    /* Used to refresh the dependency graph so that the material can be updated. */
    for (bNodeTreePath *path_iter : job_data->treepath_copy) {
      DEG_graph_id_tag_update(
          G.pr_main, depsgraph, &path_iter->nodetree->id, ID_RECALC_NTREE_OUTPUT);
    }
  }
  return true;
}

/* Called by renderer, refresh the UI. */
static void all_nodes_preview_update(void *npv, RenderResult *rr, struct rcti * /*rect*/)
{
  ShaderNodesPreviewJob *job_data = static_cast<ShaderNodesPreviewJob *>(npv);
  *job_data->do_update = true;
  if (bNode *node = job_data->rendering_node) {
    ImBuf *&image_cached = job_data->tree_previews->previews_map.lookup_or_add(node->identifier,
                                                                               nullptr);
    ImBuf *image_latest = get_image_from_viewlayer_and_pass(*rr, node->name, nullptr);
    if (image_latest == nullptr) {
      return;
    }
    if (image_cached != image_latest) {
      if (image_cached != nullptr) {
        IMB_freeImBuf(image_cached);
      }
      IMB_refImBuf(image_latest);
      image_cached = image_latest;
    }
  }
  if (job_data->rendering_AOVs) {
    for (bNode *node : job_data->AOV_nodes) {
      ImBuf *&image_cached = job_data->tree_previews->previews_map.lookup_or_add(node->identifier,
                                                                                 nullptr);
      ImBuf *image_latest = get_image_from_viewlayer_and_pass(*rr, nullptr, node->name);
      if (image_latest == nullptr) {
        continue;
      }
      if (image_cached != image_latest) {
        if (image_cached != nullptr) {
          IMB_freeImBuf(image_cached);
        }
        IMB_refImBuf(image_latest);
        image_cached = image_latest;
      }
    }
  }
}

static void preview_render(ShaderNodesPreviewJob &job_data)
{
  /* Get the stuff from the builtin preview dbase. */
  Scene *scene = preview_prepare_scene(
      job_data.bmain, job_data.scene, G.pr_main, job_data.mat_copy);
  if (scene == nullptr) {
    return;
  }
  Span<bNodeTreePath *> treepath = job_data.treepath_copy;

  /* Disconnect all input sockets of the material output node, but keep track of the displacement
   * node. */
  bNodeSocket *disp_socket = nodeFindSocket(job_data.mat_output_copy, SOCK_IN, "Displacement");
  if (disp_socket->link != nullptr) {
    job_data.mat_displacement_copy = std::make_pair(disp_socket->link->fromnode,
                                                    disp_socket->link->fromsock);
  }
  LISTBASE_FOREACH (bNodeSocket *, socket_iter, &job_data.mat_output_copy->inputs) {
    if (socket_iter->link != nullptr) {
      nodeRemLink(treepath.first()->nodetree, socket_iter->link);
    }
  }

  /* AOV nodes are rendered in the first RenderLayer so we route them now. */
  connect_nodes_to_aovs(treepath, job_data.AOV_nodes);

  /* Create the AOV passes for the viewlayer. */
  ViewLayer *AOV_layer = static_cast<ViewLayer *>(scene->view_layers.first);
  for (bNode *node : job_data.shader_nodes) {
    ViewLayer *vl = BKE_view_layer_add(scene, node->name, AOV_layer, VIEWLAYER_ADD_COPY);
    strcpy(vl->name, node->name);
  }
  for (bNode *node : job_data.AOV_nodes) {
    ViewLayerAOV *aov = BKE_view_layer_add_aov(AOV_layer);
    strcpy(aov->name, node->name);
  }
  scene->r.xsch = job_data.tree_previews->preview_size;
  scene->r.ysch = job_data.tree_previews->preview_size;
  scene->r.size = 100;

  if (job_data.tree_previews->previews_render == nullptr) {
    char name[32];
    SNPRINTF(name, "Preview %p", &job_data.tree_previews);
    job_data.tree_previews->previews_render = RE_NewRender(name);
  }
  Render *re = job_data.tree_previews->previews_render;

  /* `sce->r` gets copied in RE_InitState. */
  scene->r.scemode &= ~(R_MATNODE_PREVIEW | R_TEXNODE_PREVIEW);
  scene->r.scemode &= ~R_NO_IMAGE_LOAD;

  scene->display.render_aa = SCE_DISPLAY_AA_SAMPLES_8;

  RE_display_update_cb(re, &job_data, all_nodes_preview_update);
  RE_test_break_cb(re, &job_data, nodetree_previews_break);
  RE_prepare_viewlayer_cb(re, &job_data, prepare_viewlayer_update);

  /* Lens adjust. */
  float oldlens = reinterpret_cast<Camera *>(scene->camera->data)->lens;

  RE_ClearResult(re);
  RE_PreviewRender(re, G.pr_main, scene);

  reinterpret_cast<Camera *>(scene->camera->data)->lens = oldlens;

  /* Free the aov layers and the layers generated for each node. */
  BLI_freelistN(&AOV_layer->aovs);
  ViewLayer *vl = AOV_layer->next;
  while (vl) {
    ViewLayer *vl_rem = vl;
    vl = vl->next;
    BLI_remlink(&scene->view_layers, vl_rem);
    BKE_view_layer_free(vl_rem);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Preview job management
 * \{ */

static void update_needed_flag(const bNodeTree &nt, NestedTreePreviews &tree_previews)
{
  if (tree_previews.rendering) {
    if (nt.runtime->previews_refresh_state != tree_previews.rendering_previews_refresh_state) {
      tree_previews.restart_needed = true;
      return;
    }
  }
  else {
    if (nt.runtime->previews_refresh_state != tree_previews.cached_previews_refresh_state) {
      tree_previews.restart_needed = true;
      return;
    }
  }
  if (tree_previews.preview_size != U.node_preview_res) {
    tree_previews.restart_needed = true;
    return;
  }
}

static void shader_preview_startjob(void *customdata,
                                    bool *stop,
                                    bool *do_update,
                                    float * /*progress*/)
{
  ShaderNodesPreviewJob *job_data = static_cast<ShaderNodesPreviewJob *>(customdata);

  job_data->stop = stop;
  job_data->do_update = do_update;
  *do_update = true;
  bool size_changed = job_data->tree_previews->preview_size != U.node_preview_res;
  if (size_changed) {
    job_data->tree_previews->preview_size = U.node_preview_res;
  }

  /* Find the shader output node. */
  for (bNode *node_iter : job_data->mat_copy->nodetree->all_nodes()) {
    if (node_iter->type == SH_NODE_OUTPUT_MATERIAL && node_iter->flag & NODE_DO_OUTPUT) {
      job_data->mat_output_copy = node_iter;
      break;
    }
  }
  if (job_data->mat_output_copy == nullptr) {
    job_data->mat_output_copy = nodeAddStaticNode(
        nullptr, job_data->mat_copy->nodetree, SH_NODE_OUTPUT_MATERIAL);
  }

  bNodeTree *active_nodetree = job_data->treepath_copy.last()->nodetree;
  for (bNode *node : active_nodetree->all_nodes()) {
    if (!(node->flag & NODE_PREVIEW)) {
      /* Clear the cached preview for this node to be sure that the preview is re-rendered if
       * needed. */
      if (ImBuf **ibuf = job_data->tree_previews->previews_map.lookup_ptr(node->identifier)) {
        IMB_freeImBuf(*ibuf);
        *ibuf = nullptr;
      }
      continue;
    }
    if (node_use_aov(*node)) {
      job_data->AOV_nodes.append(node);
    }
    else {
      job_data->shader_nodes.append(node);
    }
  }

  if (job_data->tree_previews->preview_size > 0) {
    preview_render(*job_data);
  }
}

static void shader_preview_free(void *customdata)
{
  ShaderNodesPreviewJob *job_data = static_cast<ShaderNodesPreviewJob *>(customdata);
  for (bNodeTreePath *path : job_data->treepath_copy) {
    MEM_freeN(path);
  }
  job_data->treepath_copy.clear();
  job_data->tree_previews->rendering = false;
  job_data->tree_previews->cached_previews_refresh_state =
      job_data->tree_previews->rendering_previews_refresh_state;
  if (job_data->mat_copy != nullptr) {
    BLI_remlink(&G.pr_main->materials, job_data->mat_copy);
    BKE_id_free(G.pr_main, &job_data->mat_copy->id);
    job_data->mat_copy = nullptr;
  }
  MEM_delete(job_data);
}

static void ensure_nodetree_previews(const bContext &C,
                                     NestedTreePreviews &tree_previews,
                                     Material &material,
                                     ListBase &treepath)
{
  Scene *scene = CTX_data_scene(&C);
  if (!ED_check_engine_supports_preview(scene)) {
    return;
  }

  bNodeTree *displayed_nodetree = static_cast<bNodeTreePath *>(treepath.last)->nodetree;
  update_needed_flag(*displayed_nodetree, tree_previews);
  if (!(tree_previews.restart_needed)) {
    return;
  }
  if (tree_previews.rendering) {
    WM_jobs_stop(CTX_wm_manager(&C),
                 CTX_wm_space_node(&C),
                 reinterpret_cast<void *>(shader_preview_startjob));
    return;
  }
  tree_previews.rendering = true;
  tree_previews.restart_needed = false;
  tree_previews.rendering_previews_refresh_state =
      displayed_nodetree->runtime->previews_refresh_state;

  ED_preview_ensure_dbase(false);

  wmJob *wm_job = WM_jobs_get(CTX_wm_manager(&C),
                              CTX_wm_window(&C),
                              CTX_wm_space_node(&C),
                              "Shader Previews",
                              WM_JOB_EXCL_RENDER,
                              WM_JOB_TYPE_RENDER_PREVIEW);
  ShaderNodesPreviewJob *job_data = MEM_new<ShaderNodesPreviewJob>(__func__);

  job_data->scene = scene;
  job_data->tree_previews = &tree_previews;
  job_data->bmain = CTX_data_main(&C);
  job_data->mat_copy = duplicate_material(material);
  job_data->rendering_node = nullptr;
  job_data->rendering_AOVs = false;

  /* Update the treepath copied to fit the structure of the nodetree copied. */
  bNodeTreePath *root_path = MEM_cnew<bNodeTreePath>(__func__);
  root_path->nodetree = job_data->mat_copy->nodetree;
  job_data->treepath_copy.append(root_path);
  for (bNodeTreePath *original_path = static_cast<bNodeTreePath *>(treepath.first)->next;
       original_path;
       original_path = original_path->next)
  {
    bNodeTreePath *new_path = MEM_cnew<bNodeTreePath>(__func__);
    memcpy(new_path, original_path, sizeof(bNodeTreePath));
    bNode *parent = nodeFindNodebyName(job_data->treepath_copy.last()->nodetree,
                                       original_path->node_name);
    new_path->nodetree = reinterpret_cast<bNodeTree *>(parent->id);
    job_data->treepath_copy.append(new_path);
  }

  WM_jobs_customdata_set(wm_job, job_data, shader_preview_free);
  WM_jobs_timer(wm_job, 0.2, NC_NODE, NC_NODE);
  WM_jobs_callbacks(wm_job, shader_preview_startjob, nullptr, nullptr, nullptr);

  WM_jobs_start(CTX_wm_manager(&C), wm_job);
}

void stop_preview_job(wmWindowManager &wm)
{
  WM_jobs_stop(&wm, nullptr, reinterpret_cast<void *>(shader_preview_startjob));
}

void free_previews(wmWindowManager &wm, SpaceNode &snode)
{
  /* This should not be called from the drawing pass, because it will result in a deadlock. */
  WM_jobs_kill(&wm, &snode, shader_preview_startjob);
  snode.runtime->tree_previews_per_context.clear_and_shrink();
}

/** \} */

}  // namespace blender::ed::space_node

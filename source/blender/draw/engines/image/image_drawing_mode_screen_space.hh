/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * Copyright 2021, Blender Foundation.
 */

/** \file
 * \ingroup draw_engine
 */

#pragma once

#include "image_private.hh"

#include "BKE_image_partial_update.hh"

namespace blender::draw::image_engine {

constexpr float EPSILON_UV_BOUNDS = 0.00001f;

/** \brief Create GPUBatch for a IMAGE_ScreenSpaceTextureInfo. */
class BatchUpdater {
  IMAGE_ScreenSpaceTextureInfo &info;

  GPUVertFormat format = {0};
  int pos_id;
  int uv_id;

 public:
  BatchUpdater(IMAGE_ScreenSpaceTextureInfo &info) : info(info)
  {
  }

  void update_batch()
  {
    ensure_clear_batch();
    ensure_format();
    init_batch();
  }

  void discard_batch()
  {
    GPU_BATCH_DISCARD_SAFE(info.batch);
  }

 private:
  void ensure_clear_batch()
  {
    GPU_BATCH_CLEAR_SAFE(info.batch);
    if (info.batch == nullptr) {
      info.batch = GPU_batch_calloc();
    }
  }

  void init_batch()
  {
    GPUVertBuf *vbo = create_vbo();
    GPU_batch_init_ex(info.batch, GPU_PRIM_TRI_FAN, vbo, nullptr, GPU_BATCH_OWNS_VBO);
  }

  GPUVertBuf *create_vbo()
  {
    GPUVertBuf *vbo = GPU_vertbuf_create_with_format(&format);
    GPU_vertbuf_data_alloc(vbo, 4);
    float pos[4][2];
    fill_tri_fan_from_rctf(pos, info.clipping_bounds);
    float uv[4][2];
    fill_tri_fan_from_rctf(uv, info.uv_bounds);

    for (int i = 0; i < 4; i++) {
      GPU_vertbuf_attr_set(vbo, pos_id, i, pos[i]);
      GPU_vertbuf_attr_set(vbo, uv_id, i, uv[i]);
    }

    return vbo;
  }

  static void fill_tri_fan_from_rctf(float result[4][2], rctf &rect)
  {
    result[0][0] = rect.xmin;
    result[0][1] = rect.ymin;
    result[1][0] = rect.xmax;
    result[1][1] = rect.ymin;
    result[2][0] = rect.xmax;
    result[2][1] = rect.ymax;
    result[3][0] = rect.xmin;
    result[3][1] = rect.ymax;
  }

  void ensure_format()
  {
    if (format.attr_len == 0) {
      GPU_vertformat_attr_add(&format, "pos", GPU_COMP_F32, 2, GPU_FETCH_FLOAT);
      GPU_vertformat_attr_add(&format, "uv", GPU_COMP_F32, 2, GPU_FETCH_FLOAT);

      pos_id = GPU_vertformat_attr_id_get(&format, "pos");
      uv_id = GPU_vertformat_attr_id_get(&format, "uv");
    }
  }
};

/**
 * \brief Accessor to texture slots.
 *
 * Texture slots info is stored in IMAGE_PrivateData. The GPUTextures are stored in
 * IMAGE_TextureList. This class simplifies accessing texture slots by providing
 */
struct PrivateDataAccessor {
  IMAGE_PrivateData *pd;
  IMAGE_TextureList *txl;

  PrivateDataAccessor(IMAGE_PrivateData *pd, IMAGE_TextureList *txl) : pd(pd), txl(txl)
  {
  }

  /** \brief Clear dirty flag from all texture slots. */
  void clear_dirty_flag()
  {
    for (int i = 0; i < SCREEN_SPACE_DRAWING_MODE_TEXTURE_LEN; i++) {
      pd->screen_space.texture_infos[i].dirty = false;
    }
  }

  /** \brief Update the texture slot uv and screen space bounds. */
  void update_screen_space_bounds(const ARegion *region)
  {
    /* Create a single texture that covers the visible screen space. */
    BLI_rctf_init(
        &pd->screen_space.texture_infos[0].clipping_bounds, 0, region->winx, 0, region->winy);
    pd->screen_space.texture_infos[0].visible = true;

    /* Mark the other textures as invalid. */
    for (int i = 1; i < SCREEN_SPACE_DRAWING_MODE_TEXTURE_LEN; i++) {
      BLI_rctf_init_minmax(&pd->screen_space.texture_infos[i].clipping_bounds);
      pd->screen_space.texture_infos[i].visible = false;
    }
  }

  void update_uv_bounds()
  {
    /* Calculate the uv coordinates of the screen space visible corners. */
    float inverse_mat[4][4];
    DRW_view_viewmat_get(NULL, inverse_mat, true);

    rctf new_uv_bounds;
    float uv_min[3];
    static const float screen_space_co1[3] = {0.0, 0.0, 0.0};
    mul_v3_m4v3(uv_min, inverse_mat, screen_space_co1);

    static const float screen_space_co2[3] = {1.0, 1.0, 0.0};
    float uv_max[3];
    mul_v3_m4v3(uv_max, inverse_mat, screen_space_co2);
    BLI_rctf_init(&new_uv_bounds, uv_min[0], uv_max[0], uv_min[1], uv_max[1]);

    if (!BLI_rctf_compare(
            &pd->screen_space.texture_infos[0].uv_bounds, &new_uv_bounds, EPSILON_UV_BOUNDS)) {
      pd->screen_space.texture_infos[0].uv_bounds = new_uv_bounds;
      pd->screen_space.texture_infos[0].dirty = true;
      update_uv_to_texture_matrix(&pd->screen_space.texture_infos[0]);
    }

    /* Mark the other textures as invalid. */
    for (int i = 1; i < SCREEN_SPACE_DRAWING_MODE_TEXTURE_LEN; i++) {
      BLI_rctf_init_minmax(&pd->screen_space.texture_infos[i].clipping_bounds);
    }
  }

  void update_uv_to_texture_matrix(IMAGE_ScreenSpaceTextureInfo *info)
  {
    // TODO: I remember that there was a function for this somewhere.
    unit_m4(info->uv_to_texture);
    float scale_x = 1.0 / BLI_rctf_size_x(&info->uv_bounds);
    float scale_y = 1.0 / BLI_rctf_size_y(&info->uv_bounds);
    float translate_x = scale_x * -info->uv_bounds.xmin;
    float translate_y = scale_y * -info->uv_bounds.ymin;

    info->uv_to_texture[0][0] = scale_x;
    info->uv_to_texture[1][1] = scale_y;
    info->uv_to_texture[3][0] = translate_x;
    info->uv_to_texture[3][1] = translate_y;
  }

  void update_batches()
  {
    for (int i = 0; i < SCREEN_SPACE_DRAWING_MODE_TEXTURE_LEN; i++) {
      IMAGE_ScreenSpaceTextureInfo &info = pd->screen_space.texture_infos[i];
      if (!info.dirty) {
        continue;
      }
      BatchUpdater batch_updater(info);
      batch_updater.update_batch();
    }
  }
};

struct ImageTileAccessor {
  ImageTile *image_tile;
  ImageTileAccessor(ImageTile *image_tile) : image_tile(image_tile)
  {
  }

  int get_tile_number() const
  {
    return image_tile->tile_number;
  }

  int get_tile_x_offset() const
  {
    int tile_number = get_tile_number();
    return (tile_number - 1001) % 10;
  }

  int get_tile_y_offset() const
  {
    int tile_number = get_tile_number();
    return (tile_number - 1001) / 10;
  }
};

using namespace blender::bke::image::partial_update;

class ScreenSpaceDrawingMode : public AbstractDrawingMode {
 private:
  DRWPass *create_image_pass() const
  {
    /* Write depth is needed for background overlay rendering. Near depth is used for
     * transparency checker and Far depth is used for indicating the image size. */
    DRWState state = static_cast<DRWState>(DRW_STATE_WRITE_COLOR | DRW_STATE_WRITE_DEPTH |
                                           DRW_STATE_DEPTH_ALWAYS | DRW_STATE_BLEND_ALPHA_PREMUL);
    return DRW_pass_create("Image", state);
  }

  void add_shgroups(IMAGE_PassList *psl,
                    IMAGE_TextureList *txl,
                    IMAGE_PrivateData *pd,
                    const ShaderParameters &sh_params) const
  {
    GPUShader *shader = IMAGE_shader_image_get(false);

    DRWShadingGroup *shgrp = DRW_shgroup_create(shader, psl->image_pass);
    DRW_shgroup_uniform_vec2_copy(shgrp, "farNearDistances", sh_params.far_near);
    DRW_shgroup_uniform_vec4_copy(shgrp, "color", ShaderParameters::color);
    DRW_shgroup_uniform_vec4_copy(shgrp, "shuffle", sh_params.shuffle);
    DRW_shgroup_uniform_int_copy(shgrp, "drawFlags", sh_params.flags);
    DRW_shgroup_uniform_bool_copy(shgrp, "imgPremultiplied", sh_params.use_premul_alpha);
    DRW_shgroup_uniform_vec2_copy(shgrp, "maxUv", pd->screen_space.max_uv);
    float image_mat[4][4];
    unit_m4(image_mat);
    for (int i = 0; i < SCREEN_SPACE_DRAWING_MODE_TEXTURE_LEN; i++) {
      IMAGE_ScreenSpaceTextureInfo &info = pd->screen_space.texture_infos[i];
      if (!info.visible) {
        continue;
      }
      /*
        Should be space relative translation.
        image_mat[0][0] = info.clipping_bounds.xmax;
        image_mat[1][1] = info.clipping_bounds.ymax;
      */

      DRWShadingGroup *shgrp_sub = DRW_shgroup_create_sub(shgrp);
      DRW_shgroup_uniform_texture_ex(
          shgrp_sub, "imageTexture", txl->screen_space.textures[i], GPU_SAMPLER_DEFAULT);
      DRW_shgroup_call_obmat(shgrp_sub, info.batch, image_mat);
    }
  }

  /**
   * \brief check if the partial update user in the private data can still be used.
   *
   * When switching to a different image the partial update user should be recreated.
   */
  bool partial_update_is_valid(const IMAGE_PrivateData *pd, const Image *image) const
  {
    if (pd->screen_space.partial_update_image != image) {
      return false;
    }

    return pd->screen_space.partial_update_user != nullptr;
  }

  void partial_update_allocate(IMAGE_PrivateData *pd, const Image *image) const
  {
    BLI_assert(pd->screen_space.partial_update_user == nullptr);
    pd->screen_space.partial_update_user = BKE_image_partial_update_create(image);
    pd->screen_space.partial_update_image = image;
  }

  void partial_update_free(IMAGE_PrivateData *pd) const
  {
    if (pd->screen_space.partial_update_user != nullptr) {
      BKE_image_partial_update_free(pd->screen_space.partial_update_user);
      pd->screen_space.partial_update_user = nullptr;
    }
  }

  void update_texture_slot_allocation(IMAGE_TextureList *txl, IMAGE_PrivateData *pd) const
  {
    for (int i = 0; i < SCREEN_SPACE_DRAWING_MODE_TEXTURE_LEN; i++) {
      const bool is_allocated = txl->screen_space.textures[i] != nullptr;
      const bool is_visible = pd->screen_space.texture_infos[i].visible;
      const bool should_be_freed = !is_visible && is_allocated;
      const bool should_be_created = is_visible && !is_allocated;

      if (should_be_freed) {
        GPU_texture_free(txl->screen_space.textures[i]);
        txl->screen_space.textures[i] = nullptr;
      }

      if (should_be_created) {
        DRW_texture_ensure_fullscreen_2d(
            &txl->screen_space.textures[i], GPU_RGBA16F, static_cast<DRWTextureFlag>(0));
      }
      pd->screen_space.texture_infos[i].dirty |= should_be_created;
    }
  }

  void mark_all_texture_slots_dirty(IMAGE_PrivateData *pd) const
  {
    for (int i = 0; i < SCREEN_SPACE_DRAWING_MODE_TEXTURE_LEN; i++) {
      pd->screen_space.texture_infos[i].dirty = true;
    }
  }

  /**
   * \brief Update GPUTextures for drawing the image.
   *
   * GPUTextures that are marked dirty are rebuild. GPUTextures that aren't marked dirty are
   * updated with changed region of the image.
   */
  void update_textures(IMAGE_TextureList *txl,
                       IMAGE_PrivateData *pd,
                       Image *image,
                       ImageUser *image_user) const
  {
    PartialUpdateChecker<ImageTileData> checker(
        image, image_user, pd->screen_space.partial_update_user);
    PartialUpdateChecker<ImageTileData>::CollectResult changes = checker.collect_changes();

    switch (changes.get_result_code()) {
      case ePartialUpdateCollectResult::FullUpdateNeeded:
        mark_all_texture_slots_dirty(pd);
        break;
      case ePartialUpdateCollectResult::NoChangesDetected:
        break;
      case ePartialUpdateCollectResult::PartialChangesDetected:
        /* Partial update when wrap repeat is enabled is not supported. */
        if (pd->flags.do_tile_drawing) {
          mark_all_texture_slots_dirty(pd);
        }
        else {
          do_partial_update(changes, txl, pd);
        }
        break;
    }
    do_full_update_for_dirty_textures(txl, pd, image_user);
  }

  void do_partial_update(PartialUpdateChecker<ImageTileData>::CollectResult &iterator,
                         IMAGE_TextureList *txl,
                         IMAGE_PrivateData *pd) const
  {
    while (iterator.get_next_change() == ePartialUpdateIterResult::ChangeAvailable) {
      /* Quick exit when tile_buffer isn't availble. */
      if (iterator.tile_data.tile_buffer == nullptr) {
        continue;
      }
      ensure_float_buffer(*iterator.tile_data.tile_buffer);
      const float tile_width = static_cast<float>(iterator.tile_data.tile_buffer->x);
      const float tile_height = static_cast<float>(iterator.tile_data.tile_buffer->y);

      for (int i = 0; i < SCREEN_SPACE_DRAWING_MODE_TEXTURE_LEN; i++) {
        const IMAGE_ScreenSpaceTextureInfo *info = &pd->screen_space.texture_infos[i];
        /* Dirty images will receive a full update. No need to do a partial one now. */
        if (info->dirty) {
          continue;
        }
        if (!info->visible) {
          continue;
        }
        GPUTexture *texture = txl->screen_space.textures[i];
        const float texture_width = GPU_texture_width(texture);
        const float texture_height = GPU_texture_height(texture);
        // TODO
        // early bound check.
        ImageTileAccessor tile_accessor(iterator.tile_data.tile);
        float tile_offset_x = static_cast<float>(tile_accessor.get_tile_x_offset());
        float tile_offset_y = static_cast<float>(tile_accessor.get_tile_y_offset());
        rcti *changed_region_in_texel_space = &iterator.changed_region.region;
        rctf changed_region_in_uv_space;
        BLI_rctf_init(&changed_region_in_uv_space,
                      static_cast<float>(changed_region_in_texel_space->xmin) /
                              static_cast<float>(iterator.tile_data.tile_buffer->x) +
                          tile_offset_x,
                      static_cast<float>(changed_region_in_texel_space->xmax) /
                              static_cast<float>(iterator.tile_data.tile_buffer->x) +
                          tile_offset_x,
                      static_cast<float>(changed_region_in_texel_space->ymin) /
                              static_cast<float>(iterator.tile_data.tile_buffer->y) +
                          tile_offset_y,
                      static_cast<float>(changed_region_in_texel_space->ymax) /
                              static_cast<float>(iterator.tile_data.tile_buffer->y) +
                          tile_offset_y);
        rctf changed_overlapping_region_in_uv_space;
        const bool region_overlap = BLI_rctf_isect(&info->uv_bounds,
                                                   &changed_region_in_uv_space,
                                                   &changed_overlapping_region_in_uv_space);
        if (!region_overlap) {
          continue;
        }
        // convert the overlapping region to texel space and to ss_pixel space...
        // TODO: first convert to ss_pixel space as integer based. and from there go back to texel
        // space. But perhaps this isn't needed and we could use an extraction offset somehow.
        rcti gpu_texture_region_to_update;
        BLI_rcti_init(&gpu_texture_region_to_update,
                      floor((changed_overlapping_region_in_uv_space.xmin - info->uv_bounds.xmin) *
                            texture_width / BLI_rctf_size_x(&info->uv_bounds)),
                      floor((changed_overlapping_region_in_uv_space.xmax - info->uv_bounds.xmin) *
                            texture_width / BLI_rctf_size_x(&info->uv_bounds)),
                      ceil((changed_overlapping_region_in_uv_space.ymin - info->uv_bounds.ymin) *
                           texture_height / BLI_rctf_size_y(&info->uv_bounds)),
                      ceil((changed_overlapping_region_in_uv_space.ymax - info->uv_bounds.ymin) *
                           texture_height / BLI_rctf_size_y(&info->uv_bounds)));

        rcti tile_region_to_extract;
        BLI_rcti_init(
            &tile_region_to_extract,
            floor((changed_overlapping_region_in_uv_space.xmin - tile_offset_x) * tile_width),
            floor((changed_overlapping_region_in_uv_space.xmax - tile_offset_x) * tile_width),
            ceil((changed_overlapping_region_in_uv_space.ymin - tile_offset_y) * tile_height),
            ceil((changed_overlapping_region_in_uv_space.ymax - tile_offset_y) * tile_height));

        // Create an image buffer with a size
        // extract and scale into an imbuf
        const int texture_region_width = BLI_rcti_size_x(&gpu_texture_region_to_update);
        const int texture_region_height = BLI_rcti_size_y(&gpu_texture_region_to_update);

        ImBuf extracted_buffer;
        IMB_initImBuf(
            &extracted_buffer, texture_region_width, texture_region_height, 32, IB_rectfloat);

        int offset = 0;
        ImBuf *tile_buffer = iterator.tile_data.tile_buffer;
        for (int y = gpu_texture_region_to_update.ymin; y < gpu_texture_region_to_update.ymax;
             y++) {
          float yf = y / (float)texture_height;
          float v = info->uv_bounds.ymax * yf + info->uv_bounds.ymin * (1.0 - yf) - tile_offset_y;
          for (int x = gpu_texture_region_to_update.xmin; x < gpu_texture_region_to_update.xmax;
               x++) {
            float xf = x / (float)texture_width;
            float u = info->uv_bounds.xmax * xf + info->uv_bounds.xmin * (1.0 - xf) -
                      tile_offset_x;
            nearest_interpolation_color(tile_buffer,
                                        nullptr,
                                        &extracted_buffer.rect_float[offset * 4],
                                        u * tile_buffer->x,
                                        v * tile_buffer->y);
            offset++;
          }
        }

        GPU_texture_update_sub(texture,
                               GPU_DATA_FLOAT,
                               extracted_buffer.rect_float,
                               gpu_texture_region_to_update.xmin,
                               gpu_texture_region_to_update.ymin,
                               0,
                               extracted_buffer.x,
                               extracted_buffer.y,
                               0);
        imb_freerectImbuf_all(&extracted_buffer);
      }
    }
  }

  void do_full_update_for_dirty_textures(IMAGE_TextureList *txl,
                                         IMAGE_PrivateData *pd,
                                         const ImageUser *image_user) const
  {
    for (int i = 0; i < SCREEN_SPACE_DRAWING_MODE_TEXTURE_LEN; i++) {
      IMAGE_ScreenSpaceTextureInfo *info = &pd->screen_space.texture_infos[i];
      if (!info->dirty) {
        continue;
      }
      if (!info->visible) {
        continue;
      }
      GPUTexture *gpu_texture = txl->screen_space.textures[i];
      do_full_update_gpu_texture(*info, gpu_texture, pd, image_user);
    }
  }

  void do_full_update_gpu_texture(const IMAGE_ScreenSpaceTextureInfo &texture_info,
                                  GPUTexture *gpu_texture,
                                  IMAGE_PrivateData *pd,
                                  const ImageUser *image_user) const
  {

    ImBuf texture_buffer;
    const int texture_width = GPU_texture_width(gpu_texture);
    const int texture_height = GPU_texture_height(gpu_texture);
    IMB_initImBuf(&texture_buffer, texture_width, texture_height, 0, IB_rectfloat);
    ImageUser tile_user = *image_user;

    LISTBASE_FOREACH (ImageTile *, image_tile_ptr, &pd->image->tiles) {
      const ImageTileAccessor image_tile(image_tile_ptr);
      tile_user.tile = image_tile.get_tile_number();
      ImBuf *tile_buffer = BKE_image_acquire_ibuf(pd->image, &tile_user, NULL);
      if (tile_buffer == nullptr) {
        /* Couldn't load the image buffer of the tile. */
        continue;
      }
      do_full_update_texture_slot(*pd, texture_info, texture_buffer, *tile_buffer, image_tile);
      BKE_image_release_ibuf(pd->image, tile_buffer, nullptr);
    }
    GPU_texture_update(gpu_texture, GPU_DATA_FLOAT, texture_buffer.rect_float);
    imb_freerectImbuf_all(&texture_buffer);
  }

  /**
   * \brief Ensure that the float buffer of the given image buffer is available.
   *
   * TODO: Should we add a ImageBufferAccessor for cleaner access.
   * (`image_buffer.ensure_float_buffer()`)
   */
  void ensure_float_buffer(ImBuf &image_buffer) const
  {
    if (image_buffer.rect_float == nullptr) {
      IMB_float_from_rect(&image_buffer);
    }
  }

  void do_full_update_texture_slot(const IMAGE_PrivateData &pd,
                                   const IMAGE_ScreenSpaceTextureInfo &texture_info,
                                   ImBuf &texture_buffer,
                                   ImBuf &tile_buffer,
                                   const ImageTileAccessor &image_tile) const
  {
    const int texture_width = texture_buffer.x;
    const int texture_height = texture_buffer.y;
    ensure_float_buffer(tile_buffer);

    /* IMB_transform works in a non-consistent space. This should be documented or fixed!.
     * Construct a variant of the info_uv_to_texture that adds the texel space
     * transformation.*/
    float uv_to_texel[4][4];
    copy_m4_m4(uv_to_texel, texture_info.uv_to_texture);
    float scale[3] = {static_cast<float>(texture_width) / static_cast<float>(tile_buffer.x),
                      static_cast<float>(texture_height) / static_cast<float>(tile_buffer.y),
                      1.0f};
    rescale_m4(uv_to_texel, scale);
    uv_to_texel[3][0] += image_tile.get_tile_x_offset() / BLI_rctf_size_x(&texture_info.uv_bounds);
    uv_to_texel[3][1] += image_tile.get_tile_y_offset() / BLI_rctf_size_y(&texture_info.uv_bounds);
    uv_to_texel[3][0] *= texture_width;
    uv_to_texel[3][1] *= texture_height;
    invert_m4(uv_to_texel);

    rctf crop_rect;
    rctf *crop_rect_ptr = nullptr;
    /* TODO: use regular when drawing none repeating single tile buffers. */
    eIMBTransformMode transform_mode;  // = IMB_TRANSFORM_MODE_REGULAR;
    if (pd.flags.do_tile_drawing) {
      transform_mode = IMB_TRANSFORM_MODE_WRAP_REPEAT;
    }
    else {
      BLI_rctf_init(&crop_rect, 0.0, tile_buffer.x, 0.0, tile_buffer.y);
      crop_rect_ptr = &crop_rect;
      transform_mode = IMB_TRANSFORM_MODE_CROP_SRC;
    }

    IMB_transform(&tile_buffer,
                  &texture_buffer,
                  transform_mode,
                  IMB_FILTER_NEAREST,
                  uv_to_texel,
                  crop_rect_ptr);
  }

 public:
  void cache_init(IMAGE_Data *vedata) const override
  {
    IMAGE_PassList *psl = vedata->psl;

    psl->image_pass = create_image_pass();
  }

  void cache_image(AbstractSpaceAccessor *space,
                   IMAGE_Data *vedata,
                   Image *image,
                   ImageUser *iuser,
                   ImBuf *image_buffer) const override
  {
    const DRWContextState *draw_ctx = DRW_context_state_get();
    IMAGE_PassList *psl = vedata->psl;
    IMAGE_TextureList *txl = vedata->txl;
    IMAGE_StorageList *stl = vedata->stl;
    IMAGE_PrivateData *pd = stl->pd;
    PrivateDataAccessor pda(pd, txl);

    if (!partial_update_is_valid(pd, image)) {
      partial_update_free(pd);
      partial_update_allocate(pd, image);
    }

    copy_v2_fl2(pd->screen_space.max_uv, 1.0f, 1.0);
    LISTBASE_FOREACH (ImageTile *, image_tile, &image->tiles) {
      ImageTileAccessor image_tile_accessor(image_tile);
      pd->screen_space.max_uv[0] = max_ii(pd->screen_space.max_uv[0],
                                          image_tile_accessor.get_tile_x_offset() + 1);
      pd->screen_space.max_uv[1] = max_ii(pd->screen_space.max_uv[1],
                                          image_tile_accessor.get_tile_y_offset() + 1);
    }

    // Step: Find out which screen space textures are needed to draw on the screen. Remove the
    // screen space textures that aren't needed.
    const ARegion *region = draw_ctx->region;
    pda.clear_dirty_flag();
    pda.update_screen_space_bounds(region);
    pda.update_uv_bounds();
    pda.update_batches();
    update_texture_slot_allocation(txl, pd);

    // Step: Update the GPU textures based on the changes in the image.
    update_textures(txl, pd, image, iuser);

    // Step: Add the GPU textures to the shgroup.
    ShaderParameters sh_params;
    sh_params.use_premul_alpha = BKE_image_has_gpu_texture_premultiplied_alpha(image,
                                                                               image_buffer);

    const Scene *scene = draw_ctx->scene;
    if (scene->camera && scene->camera->type == OB_CAMERA) {
      Camera *camera = static_cast<Camera *>(scene->camera->data);
      copy_v2_fl2(sh_params.far_near, camera->clip_end, camera->clip_start);
    }
    const bool is_tiled_image = (image->source == IMA_SRC_TILED);
    space->get_shader_parameters(sh_params, image_buffer, is_tiled_image);

    add_shgroups(psl, txl, pd, sh_params);
  }

  void draw_finish(IMAGE_Data *UNUSED(vedata)) const override
  {
  }

  void draw_scene(IMAGE_Data *vedata) const override
  {
    IMAGE_PassList *psl = vedata->psl;
    IMAGE_PrivateData *pd = vedata->stl->pd;

    DefaultFramebufferList *dfbl = DRW_viewport_framebuffer_list_get();
    GPU_framebuffer_bind(dfbl->default_fb);
    static float clear_col[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    GPU_framebuffer_clear_color_depth(dfbl->default_fb, clear_col, 1.0);

    DRW_view_set_active(pd->view);
    DRW_draw_pass(psl->image_pass);
    DRW_view_set_active(nullptr);
  }
};  // namespace clipping

}  // namespace blender::draw::image_engine
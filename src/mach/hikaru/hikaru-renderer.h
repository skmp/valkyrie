/* 
 * Valkyrie
 * Copyright (C) 2011, Stefano Teso
 * 
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
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

#ifndef __VK_HIKARU_RENDERER_H__
#define __VK_HIKARU_RENDERER_H__

#include "vk/core.h"
#include "vk/types.h"
#include "vk/buffer.h"
#include "vk/renderer.h"
#include "mach/hikaru/hikaru-gpu-private.h"

vk_renderer_t	*hikaru_renderer_new (vk_buffer_t *texram);

void		hikaru_renderer_set_viewport (vk_renderer_t *renderer,
		                              hikaru_gpu_viewport_t *viewport);
void		hikaru_renderer_set_matrix (vk_renderer_t *renderer,
		                            mtx4x4f_t *mtx);
void		hikaru_renderer_set_material (vk_renderer_t *renderer,
 		                              hikaru_gpu_material_t *material);
void		hikaru_renderer_set_texhead (vk_renderer_t *renderer,
		                             hikaru_gpu_texhead_t *texhead);
void		hikaru_renderer_set_light (vk_renderer_t *renderer,
		                           hikaru_gpu_light_t *light);
void		hikaru_renderer_set_modelview_vertex (vk_renderer_t *renderer,
		                                      unsigned m, unsigned n,
		                                      vec3f_t *vertex);
void		hikaru_renderer_append_vertex (vk_renderer_t *renderer,
		                               vec3f_t *pos);
void		hikaru_renderer_append_texcoords (vk_renderer_t *renderer,
		                                  vec2f_t *texcoords);
void		hikaru_renderer_append_vertex_full (vk_renderer_t *renderer,
		                                    vec3f_t *pos,
		                                    vec3f_t *normal,
		                                    vec2f_t *texcoords);
void		hikaru_renderer_end_vertex_data (vk_renderer_t *renderer);

void		hikaru_renderer_draw_layer (vk_renderer_t *renderer,
		                            uint32_t x0, uint32_t y0,
		                            uint32_t x1, uint32_t y1);

void		hikaru_renderer_register_texture (vk_renderer_t *renderer,
		                                  hikaru_gpu_texture_t *texture);

#endif /* __VK_HIKARU_RENDERER_H__ */

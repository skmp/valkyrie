/* 
 * Valkyrie
 * Copyright (C) 2011-2013, Stefano Teso
 * 
 * Valkyrie is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Valkyrie is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Valkyrie.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef __HIKARU_GPU_PRIVATE_H__
#define __HIKARU_GPU_PRIVATE_H__

#include "vk/device.h"

#define NUM_VIEWPORTS	8
#define NUM_MODELVIEWS	256
#define NUM_MATERIALS	16384
#define NUM_TEXHEADS	16384
#define NUM_LIGHTS	1024
#define NUM_LIGHTSETS	256

#define MAX_VERTICES_PER_MESH	16384

/* PHARRIER includes some debug text providing names for (some of the) GPU
 * IRQs. In particular, the menu at PH:@0C1D4E38 references both text (located
 * at PH:@0C126DC4 and beyond) and pointers to counter/timer variables (located
 * at PH:@0C27D0BC). These variables are updated through the function at
 * @0C0CF020, which is conveniently called from the GPU IRQ handlers (stored at
 * 0C000A04). Hence the connection. */

#define GPU15_IRQ_IDMA_END		0x01
#define GPU15_IRQ_POLY_OUT_END		0x02
#define GPU15_IRQ_CMD_ANALYSIS_END	0x04
#define GPU15_IRQ_GPU1A			0x80

#define GPU1A_IRQ_VBLANK		0x02
#define GPU1A_IRQ_PLOT_END		0x04

/* Used for both texheads and layers */
enum {
	HIKARU_FORMAT_ABGR1555 = 0,
	HIKARU_FORMAT_ABGR4444 = 1,
	HIKARU_FORMAT_ABGR1111 = 2,
	HIKARU_FORMAT_LA8 = 4,
	HIKARU_FORMAT_A2BGR10 = 8,
};

/* PHARRIER lists the following polygon types:
 *
 * opaque, shadow A, shadow B, transparent, background, translucent.
 *
 * Each of them is associated to a GPU HW performance counter, so the hardware
 * very likely distinguishes between them too. I can't be sure, but the
 * enumeration below looks so fitting...
 */

enum {
	HIKARU_POLYTYPE_OPAQUE = 1,
	HIKARU_POLYTYPE_SHADOW_A = 2,
	HIKARU_POLYTYPE_SHADOW_B = 3,
	HIKARU_POLYTYPE_TRANSPARENT = 4, /* DreamZzz calls it punchthrough */
	HIKARU_POLYTYPE_BACKGROUND = 5,
	HIKARU_POLYTYPE_TRANSLUCENT = 6
};

typedef struct {
	struct {
		float l, r;
		float b, t;
		float f, n, f2;
	} clip;
	struct {
		float x, y;
	} offset;
	struct {
		float max, min;
		float density, bias;
		vec4b_t mask;
		uint32_t func		: 3;
		uint32_t q_type		: 2;
		uint32_t q_enabled	: 1;
		uint32_t q_unknown	: 1;
	} depth;
	struct {
		vec4b_t clear;
		vec3s_t ambient;
	} color;
	union {
		struct {
			uint32_t has_021	: 1;
			uint32_t has_221	: 1;
			uint32_t has_421	: 1;
			uint32_t has_621	: 1;
			uint32_t has_011	: 1;
			uint32_t has_191	: 1;
		};
		uint32_t flags;
	};
} hikaru_viewport_t;

typedef struct {
	mtx4x4f_t mtx;
} hikaru_modelview_t;

typedef struct {
	vec3b_t diffuse;
	vec3b_t ambient;
	vec4b_t specular;
	vec3s_t unknown;
	union {
		struct {
			uint32_t		: 12;
			uint32_t unk1		: 1;
			uint32_t		: 3;
			uint32_t unk2		: 4;
			uint32_t		: 12;
		};
		uint32_t _081;
	};
	union {
		struct {
			uint32_t		: 16;
			uint32_t shading_mode	: 2;
			uint32_t depth_blend	: 1;
			uint32_t has_texture	: 1;
			uint32_t has_alpha	: 1;
			uint32_t has_highlight	: 1;
			uint32_t unk3		: 1;
			uint32_t unk4		: 1;
			uint32_t unk5		: 8;
		};
		uint32_t _881;
	};
	union {
		struct {
			uint32_t		: 16;
			uint32_t blending_mode	: 2;
			uint32_t		: 14;
		};
		uint32_t _A81;
	};
	union {
		struct {
			uint32_t		: 16;
			uint32_t alpha_index	: 6;
			uint32_t		: 10;
		};
		uint32_t _C81;
	};
	union {
		struct {
			uint32_t has_091	: 1;
			uint32_t has_291	: 1;
			uint32_t has_491	: 1;
			uint32_t has_691	: 1;
			uint32_t has_081	: 1;
			uint32_t has_881	: 1;
			uint32_t has_A81	: 1;
			uint32_t has_C81	: 1;
		};
		uint32_t flags;
	};
} hikaru_material_t;

typedef struct {
	union {
		struct {
			uint32_t 	: 16;
			uint32_t unk1	: 2;
			uint32_t 	: 2;
			uint32_t unk2	: 4;
			uint32_t unk3	: 4;
			uint32_t 	: 4;
		};
		uint32_t _0C1;
	};
	union {
		struct {
			uint32_t	: 14;
			uint32_t unk4	: 1;
			uint32_t unk5	: 1;
			uint32_t logw	: 3;
			uint32_t logh	: 3;
			uint32_t wrapu	: 1;
			uint32_t wrapv	: 1;
			uint32_t repeatu: 1;
			uint32_t repeatv: 1;
			uint32_t format	: 3;
			uint32_t unk6	: 3;
		};
		uint32_t _2C1;
	};
	union {
		struct {
			uint32_t	: 12;
			uint32_t bank	: 1;
			uint32_t	: 3;
			uint32_t slotx	: 8;
			uint32_t sloty	: 8;
		};
		uint32_t _4C1;
	};
	union {
		struct {
			uint32_t has_0C1	: 1;
			uint32_t has_2C1	: 1;
			uint32_t has_4C1	: 1;
			uint32_t has_mipmap	: 1;
		};
		uint32_t flags;
	};
} hikaru_texhead_t;

typedef enum {
	HIKARU_LIGHT_TYPE_DIRECTIONAL,
	HIKARU_LIGHT_TYPE_POSITIONAL,
	HIKARU_LIGHT_TYPE_SPOT,

	HIKARU_NUM_LIGHT_TYPES
} hikaru_light_type_t;

typedef enum {
	HIKARU_LIGHT_ATT_LINEAR = 0,
	HIKARU_LIGHT_ATT_SQUARE = 1,
	HIKARU_LIGHT_ATT_INVLINEAR = 2,
	HIKARU_LIGHT_ATT_INVSQUARE = 3,
	HIKARU_LIGHT_ATT_INF = 4,

	HIKARU_NUM_LIGHT_ATTS
} hikaru_light_att_t;

typedef struct {
	vec3f_t position;
	vec3f_t direction;
	vec2f_t attenuation;
	vec3s_t diffuse;
	vec3b_t specular;
	union {
		struct {
			uint32_t has_061	: 1;
			uint32_t has_051	: 1;
			uint32_t has_451	: 1;
			uint32_t has_position	: 1;
			uint32_t has_direction	: 1;
			uint32_t att_type	: 2;
			uint32_t _051_index	: 4;
			uint32_t _051_bit	: 1;
			uint32_t has_specular	: 1;
		};
		uint32_t flags;
	};
} hikaru_light_t;

typedef struct {
	hikaru_light_t lights[4];
	union {
		struct {
			uint32_t set		: 1;
			uint32_t mask		: 4;
		};
		uint32_t flags;
	};
} hikaru_lightset_t;

typedef struct {
	uint32_t x0, y0, x1, y1;
	uint32_t format		: 4;
	uint32_t enabled	: 1;
} hikaru_layer_t;

typedef struct {
	vk_device_t base;

	vk_buffer_t *cmdram;
	vk_buffer_t *texram[2];
	vk_buffer_t *fb;

	vk_renderer_t *renderer;

	struct {
		uint8_t _15[0x100];
		uint8_t _18[0x100];
		uint8_t _1A[0x104];
		uint8_t _1A_unit[2][0x40];
		uint8_t _1A_dma[0x10];
		uint16_t _00400000;
	} regs;

	struct {
		uint32_t pc;
		uint32_t sp[2];
		uint32_t is_running	: 1;
		uint32_t is_unhandled	: 1;
	} cp;

	struct {
		union {
			struct {
				uint32_t	: 16;
				uint32_t alpha	: 8;
				uint32_t blend	: 1;
				uint32_t	: 7;
			};
			uint32_t _181;
		};
		union {
			struct {
				uint32_t		: 16;
				uint32_t num_1		: 2;
				uint32_t enabled_1	: 1;
				uint32_t		: 5;
				uint32_t num_2		: 2;
				uint32_t enabled_2	: 1;
				uint32_t		: 5;
			};
			uint32_t _781;
		};
		vec4b_t clear;
	} fb_config;

	uint16_t depth_bias;
	uint16_t _501;
	uint16_t texoffset_x, texoffset_y;

	struct {
		uint32_t in_mesh	: 1;

		struct {
			float value;
			bool cond;
			uint16_t branch_id;
		} lod;

		struct {
			uint32_t type	: 3;
			float alpha;
			float static_mesh_precision;
		} poly;

		struct {
			hikaru_viewport_t table[NUM_VIEWPORTS];
			hikaru_viewport_t scratch;
			hikaru_viewport_t stack[32];
			int32_t depth;
		} viewports;
	
		struct {
			hikaru_modelview_t table[NUM_MODELVIEWS];
			uint32_t depth, total;
		} modelviews;
	
		struct {
			hikaru_material_t table[NUM_MATERIALS];
			hikaru_material_t scratch;
			uint32_t base;
		} materials;
	
		struct {
			hikaru_texhead_t table[NUM_TEXHEADS];
			hikaru_texhead_t scratch;
			uint32_t base;
		} texheads;
	
		struct {
			hikaru_lightset_t sets[NUM_LIGHTSETS];
			hikaru_lightset_t scratchset;
			hikaru_light_t table[NUM_LIGHTS];
			hikaru_light_t scratch;
			uint32_t base;
		} lights;

		union {
			struct {
				uint32_t lo : 8;
				uint32_t hi : 24;
			} part;
			uint32_t full;
		} alpha_table[0x40];

		union {
			struct {
				uint32_t hi : 16;
				uint32_t lo : 16;
			};
			uint32_t full;
		} light_table[4][0x20];

		struct {
			hikaru_layer_t layer[2][2];
			bool enabled;
		} layers;

	} state;

	struct {
		uint32_t log_dma	: 1;
		uint32_t log_idma	: 1;
		uint32_t log_cp		: 1;
	} debug;

} hikaru_gpu_t;

#define REG15(addr_)	(*(uint32_t *) &gpu->regs._15[(addr_) & 0xFF])
#define REG18(addr_)	(*(uint32_t *) &gpu->regs._18[(addr_) & 0xFF])
#define REG1A(addr_)	(*(uint32_t *) &gpu->regs._1A[(addr_) & 0x1FF])
#define REG1AUNIT(n,a)	(*(uint32_t *) &gpu->regs._1A_unit[n][(a) & 0x3F])
#define REG1ADMA(a)	(*(uint32_t *) &gpu->regs._1A_dma[(a) & 0xF])

#define PC		gpu->cp.pc
#define SP(i_)		gpu->cp.sp[i_]
#define UNHANDLED	gpu->cp.is_unhandled

#define LOD		gpu->state.lod
#define POLY		gpu->state.poly
#define VP		gpu->state.viewports
#define MV		gpu->state.modelviews
#define MAT		gpu->state.materials
#define TEX		gpu->state.texheads
#define LIT		gpu->state.lights
#define ATABLE		gpu->state.alpha_table
#define LTABLE		gpu->state.light_table
#define LAYERS		gpu->state.layers

/****************************************************************************
 Renderer
****************************************************************************/

#define HR_PUSH_POS	(1 << 0)
#define HR_PUSH_NRM	(1 << 1)
#define HR_PUSH_TXC	(1 << 2)

#pragma pack(push)
#pragma pack(1)

typedef union {
	struct {
		uint32_t winding	: 1; /* 0x00000001 */
		uint32_t ppivot		: 1; /* 0x00000002 */
		uint32_t tpivot		: 1; /* 0x00000004 */
		uint32_t		: 6;
		uint32_t tricap		: 3; /* 0x00000E00 */
		uint32_t unknown1	: 1; /* 0x00001000 */
		uint32_t unknown2	: 3; /* 0x0000E000 */
		uint32_t		: 2;
		uint32_t unknown3	: 1; /* 0x00040000 */
		uint32_t twosided	: 1; /* 0x00080000 */
		uint32_t		: 3;
		uint32_t nocull		: 1; /* 0x00800000 */
		uint32_t alpha		: 8; /* 0xFF000000 */
	};
	uint32_t full;
} hikaru_vertex_info_t;

typedef struct {
	vec3f_t position;	/* 0x00 */
	vec3b_t diffuse;	/* 0x0C */
	uint8_t alpha;		/* 0x0F */
	vec3f_t normal;		/* 0x10 */
	vec3b_t ambient;	/* 0x1C */
	uint8_t padding3;
	vec2f_t texcoords;	/* 0x20 */
	vec4b_t specular;	/* 0x28 */
	uint32_t padding2;
	vec3s_t unknown;	/* 0x30 */
	uint16_t padding4;
} hikaru_vertex_body_t;

typedef struct {
	hikaru_vertex_info_t info;
	hikaru_vertex_body_t body;
} hikaru_vertex_t;

#pragma pack(pop)

/****************************************************************************
 Definitions
****************************************************************************/

/* hikaru-gpu-private.c */
void get_texhead_coords (uint32_t *, uint32_t *, hikaru_texhead_t *);
const char *get_viewport_str (hikaru_viewport_t *);
const char *get_modelview_str (hikaru_modelview_t *);
const char *get_material_str (hikaru_material_t *);
const char *get_texhead_str (hikaru_texhead_t *);
const char *get_light_str (hikaru_light_t *);
const char *get_lightset_str (hikaru_lightset_t *);
const char *get_vertex_str (hikaru_vertex_t *);
const char *get_layer_str (hikaru_layer_t *);

/* hikaru-gpu.c */
void hikaru_gpu_raise_irq (hikaru_gpu_t *gpu, uint32_t _15, uint32_t _1A);

/* hikaru-gpu-cp.c */
void hikaru_gpu_cp_init (hikaru_gpu_t *);
void hikaru_gpu_cp_exec (hikaru_gpu_t *, int cycles);
void hikaru_gpu_cp_vblank_in (hikaru_gpu_t *);
void hikaru_gpu_cp_vblank_out (hikaru_gpu_t *);
void hikaru_gpu_cp_on_put (hikaru_gpu_t *);

/* hikaru-renderer.c */
void hikaru_renderer_begin_mesh (vk_renderer_t *rend, uint32_t addr,
                                 bool is_static);
void hikaru_renderer_end_mesh (vk_renderer_t *rend, uint32_t addr);
void hikaru_renderer_push_vertices (vk_renderer_t *rend,
                                    hikaru_vertex_t *v,
                                    uint32_t push,
                                    unsigned num);

void		 hikaru_renderer_invalidate_texcache (vk_renderer_t *rend,
		                                      hikaru_texhead_t *th);

#endif /* __HIKARU_GPU_PRIVATE_H__ */

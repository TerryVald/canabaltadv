#define MUSIC 0

#include <tonc.h>
#include <krawall.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "main.h"

#include "tiles/buildings.h"
#include "tiles/midground.h"
#include "tiles/background.h"
#include "tiles/sprites.h"

#include "engine.h"

uint32_t frame_count;

build_t builds[BUILDS_AHEAD];
uint32_t starts[BUILDS_AHEAD];
uint32_t curr_build = 0;

crate_t crates[MAX_CRATES];
int crate_head = 0;
int crate_tail = 0;
int num_crates = 0;

#define CURR_BUILD (builds[curr_build])
#define NEXT_BUILD (builds[(curr_build + 1) % BUILDS_AHEAD])
#define LAST_BUILD (builds[(curr_build + (BUILDS_AHEAD-1)) % BUILDS_AHEAD])

camera_t cam;
player_t guy;

OBJ_ATTR *score_objs[8];

// Game state
typedef enum {
	STATE_TITLE,
	STATE_PLAYING,
	STATE_PAUSED,
	STATE_GAME_OVER
} game_state_t;

game_state_t game_state = STATE_TITLE;
uint32_t title_blink_counter = 0;

static uint se_index_fast(uint tx, uint ty, u16 bgcnt) {
	uint n= tx + ty*32;
	if(tx >= 32)
		n += 0x03E0;
	if(ty >= 32 && (bgcnt&BG_REG_64x64)==BG_REG_64x64)
		n += 0x0400;
	return n;
}

static build_t generate_build() {
	uint8_t height = 14 + (rand() % 10 - 5);
	uint8_t width = 50 + (rand() % 10 - 5);
	uint8_t gap = 15 + (rand() % 8 - 4);
	build_style_t style = BUILD_S0;
	
	return (build_t){height, width, gap, style};
}

static void draw_col(SCR_ENTRY* map, uint32_t col) {
	if (starts[curr_build] > col) {
		return;
	}
	for (int i = 0; i < BUILDS_AHEAD; i++) {
		int bn = (curr_build + i) % BUILDS_AHEAD;
		build_t *b = &builds[bn];
		if ((starts[bn] <= col) && (col < starts[bn] + b->width + b->gap)) {
			uint16_t tile = TILE_TRANSPARENT;
			for (int row = 0; row < WORLD_HEIGHT; row++) {
				if (row > (WORLD_HEIGHT - b->height)) {
					if (col == starts[bn]) {
						tile = b->style.bl;
					} else if ((col > starts[bn]) && (col < starts[bn] + b->width)) {
						tile = b->style.bm;
					} else if (col == starts[bn] + b->width) {
						tile = b->style.br;
					}
				} else if (row == (WORLD_HEIGHT - b->height)) {
					if (col == starts[bn]) {
						tile = b->style.tl;
					} else if ((col > starts[bn]) && (col < starts[bn] + b->width)) {
						tile = b->style.tm;
					} else if (col == starts[bn] + b->width) {
						tile = b->style.tr;
					}
				} else {
					tile = TILE_TRANSPARENT;
				}
				map[se_index_fast(col%64, row, REG_BG0CNT)] = SE_PALBANK(BUILDINGS_PB) | tile;
			}
			return;
		}
	}
}

static void set_score(uint32_t score) {
	for (int i = 1; i < 8; i++) {
		obj_set_attr(score_objs[i],
		             ATTR0_TALL | ATTR0_REG,
		             ATTR1_SIZE_8,
		             ATTR2_PALBANK(0) | (TILE_ZERO + 2*(score % 10)));
		score /= 10;
		obj_set_pos(score_objs[i], SCREEN_WIDTH - PIXEL(i+1), 0);
	}
}

static void draw_title_screen() {
	// Simple title screen using text
	tte_init_se_default(0, BG_CBB(2)|BG_SBB(28));
	tte_set_pos(32, 60);
	tte_write("#{P:48,60}CANABALT");
	tte_set_pos(32, 100);
	tte_write("#{P:56,100}Press A to start");
}

static void draw_pause_screen() {
	tte_init_se_default(0, BG_CBB(2)|BG_SBB(28));
	tte_set_pos(32, 70);
	tte_write("#{P:80,70}PAUSED");
	tte_set_pos(32, 90);
	tte_write("#{P:56,90}Press Start to resume");
}

static void clear_title_screen() {
	// Clear the text layer
	tte_erase_screen();
}

int main(void) {
	// set up irqs
	fnptr isr = NULL;
	irq_init(isr);
	irq_add(II_VBLANK, isr);
	
	// set up backgrounds tiles
	dma3_cpy(pal_bg_bank[0], buildingsPal, buildingsPalLen);
	dma3_cpy(pal_bg_bank[1], midgroundPal, midgroundPalLen);
	dma3_cpy(pal_bg_bank[2], backgroundPal, backgroundPalLen);
	dma3_cpy(&tile_mem[0][0], buildingsTiles, buildingsTilesLen);
	dma3_cpy(&tile_mem[1][0], midgroundTiles, midgroundTilesLen);
	dma3_cpy(&tile_mem[2][0], backgroundTiles, backgroundTilesLen);

	SCR_ENTRY *bg0_map = se_mem[30];
	SCR_ENTRY *bg1_map = se_mem[28];
	SCR_ENTRY *bg2_map = se_mem[26];

	// load in the sprites
	init_sprites();
	memcpy(pal_obj_mem, spritesPal, spritesPalLen);
	memcpy(&tile_mem[4][0], spritesTiles, spritesTilesLen);

 reset:
	game_state = STATE_TITLE;
	
	// Set up title screen
	REG_DISPCNT = DCNT_MODE0 | DCNT_BG1 | DCNT_BG2;
	
	// Fill backgrounds for title
	REG_BG1CNT = BG_PRIO(0) | BG_CBB(1) | BG_SBB(28) | BG_4BPP | BG_REG_64x32;
	REG_BG2CNT = BG_PRIO(1) | BG_CBB(2) | BG_SBB(26) | BG_4BPP | BG_REG_64x32;
	for (int i = 0; i < BG0_WIDTH; i++) {
		for (int j = 0; j < BG0_HEIGHT; j++) {
			bg1_map[se_index_fast(i, j, REG_BG1CNT)] = SE_PALBANK(MIDGROUND_PB) | midgroundMap[64*j+i];
			bg2_map[se_index_fast(i, j, REG_BG2CNT)] = SE_PALBANK(BACKGROUND_PB) | backgroundMap[64*j+i];
		}
	}
	
	draw_title_screen();
	
	// Title screen loop
	while (game_state == STATE_TITLE) {
		VBlankIntrWait();
		key_poll();
		
		title_blink_counter++;
		
		if (key_hit(KEY_A)) {
			clear_title_screen();
			game_state = STATE_PLAYING;
			break;
		}
	}
	
	// Generate the starting buildings
	curr_build = 0;
	starts[0] = 0;
	builds[0] = generate_build();
	for (int i = 1; i < BUILDS_AHEAD; i++) {
		starts[i] = starts[i-1] + builds[i-1].width + builds[i-1].gap;
		builds[i] = generate_build();
	}

	// Fill in the initial bg0
	REG_BG0CNT = BG_PRIO(0) | BG_CBB(0) | BG_SBB(30) | BG_4BPP | BG_REG_64x32;
	for (int col = 0; col <= (SCREEN_WIDTH >> 3); col++) {
		draw_col(bg0_map, col);
	}

	// Fill in the midground bg1 and background bg2
	REG_BG1CNT = BG_PRIO(1) | BG_CBB(1) | BG_SBB(28) | BG_4BPP | BG_REG_64x32;
	REG_BG2CNT = BG_PRIO(2) | BG_CBB(2) | BG_SBB(26) | BG_4BPP | BG_REG_64x32;
	for (int i = 0; i < BG0_WIDTH; i++) {
		for (int j = 0; j < BG0_HEIGHT; j++) {
			bg1_map[se_index_fast(i, j, REG_BG1CNT)] = SE_PALBANK(MIDGROUND_PB) | midgroundMap[64*j+i];
			bg2_map[se_index_fast(i, j, REG_BG2CNT)] = SE_PALBANK(BACKGROUND_PB) | backgroundMap[64*j+i];
		}
	}

	REG_DISPCNT = DCNT_MODE0 | DCNT_BG0 | DCNT_BG1 | DCNT_BG2 | DCNT_OBJ | DCNT_OBJ_1D;
	
	// Set the starting sprites
	cam = (camera_t){0, 96, 5, 5};
	guy.s.obj = NULL;
	guy.s.x = 30; guy.s.height = PIXEL(CURR_BUILD.height) + 30;
	guy.vx = 0; guy.vy = 0; guy.ax = 0; guy.ay = 0;
	guy.state = RUN; guy.anim = ANIM_RUN;
	guy.s.shape = ATTR0_TALL;
	guy.s.size = ATTR1_SIZE_8;
	guy.s.palbank = 0;
	guy.s.tile = guy.anim.start;
	
	// Score sprites
	for (int i = 0; i < 8; i++) {
		score_objs[i] = new_obj();
		obj_set_attr(score_objs[i],
		             ATTR0_TALL | ATTR0_REG,
		             ATTR1_SIZE_8,
		             ATTR2_PALBANK(0) | TILE_ZERO);
	}
	set_score(0);
	obj_set_attr(score_objs[0], 
	             ATTR0_TALL | ATTR0_REG,
	             ATTR1_SIZE_8,
	             ATTR2_PALBANK(0) | TILE_METER_MARK);
	obj_set_pos(score_objs[0], SCREEN_WIDTH - PIXEL(1), 0);

	for (int i = 0; i < MAX_CRATES; i++) {
		crates[i].valid = false;
	}

	// Scroll around some
	int horizon = (SCREEN_WIDTH >> 3);
	int ground = PIXEL(CURR_BUILD.height);

	while(1) {
		VBlankIntrWait();
		key_poll();

		// Gestion de la pause avec Start
		if (key_hit(KEY_START)) {
			if (game_state == STATE_PLAYING) {
				game_state = STATE_PAUSED;
				draw_pause_screen();
			} else if (game_state == STATE_PAUSED) {
				game_state = STATE_PLAYING;
				clear_title_screen();
			}
		}

		// Ne pas mettre à jour le jeu si en pause
		if (game_state == STATE_PAUSED) {
			copy_objs();
			continue;
		}

		// AUTO-RUN: Le personnage avance automatiquement
		int cam_delta_x = cam.vx;  // Toujours avance à vitesse constante
		cam.x += cam_delta_x;
		guy.s.x += cam_delta_x;
		
		int jump_requested = key_hit(KEY_A);
		int ascent_ended = key_released(KEY_A);

		// things we need to do when the player moves horizontally
		if (cam_delta_x > 0) {
			for (int i = 0; i < MAX_CRATES; i++) {
				if ((!crates[i].hit) && (crates[i].s.obj != NULL)) {
					if ((guy.s.x > crates[i].s.x) &&
					    (guy.s.x < crates[i].s.x + SPRITE_WIDTH(crates[i].s)) &&
					    (guy.s.height - SPRITE_HEIGHT(guy.s) < crates[i].s.height) &&
					    (guy.s.height > crates[i].s.height - SPRITE_HEIGHT(crates[i].s))) {
						crates[i].hit = true;
						
						crates[i].vx = CRATE_HIT_VX;
						crates[i].vy = CRATE_HIT_VY;
						crates[i].ay = CRATE_GRAV;
					}
				}
			}
			
			// check if we stepped off the edge
			if ((guy.s.x >> 3) > starts[curr_build] + CURR_BUILD.width) { ground = -100; }
			// check if we just entered a new building
			if ((guy.s.x >> 3) + 1 >= starts[curr_build] + CURR_BUILD.width + CURR_BUILD.gap) {
				// generate new building and advance
				starts[curr_build] = starts[(curr_build + (BUILDS_AHEAD-1)) % BUILDS_AHEAD] + LAST_BUILD.width + LAST_BUILD.gap;
				builds[curr_build] = generate_build();

				// create crate objects if need to
				crates[crate_tail].hit = false;
				crates[crate_tail].vx = 0;
				crates[crate_tail].vy = 0;
				crates[crate_tail].ay = 0;
				crates[crate_tail].s.obj = NULL;
				crates[crate_tail].s.x = PIXEL(starts[curr_build]) + 100;
				crates[crate_tail].s.height = PIXEL(CURR_BUILD.height) + PIXEL(1);
				crates[crate_tail].s.shape = ATTR0_SQUARE;
				crates[crate_tail].s.size = ATTR1_SIZE_8;
				crates[crate_tail].s.palbank = 0;
				crates[crate_tail].s.tile = TILE_METER_MARK + 1;
				crates[crate_tail].valid = true;
				DEBUGFMT("creating a crate at %d, %d", crates[crate_tail].s.x, crates[crate_tail].s.height);
				
				curr_build = (curr_build + 1) % BUILDS_AHEAD;
				crate_tail = (crate_tail + 1) % MAX_CRATES;
				num_crates++;

				// update ground level
				ground = PIXEL(CURR_BUILD.height);
			}

			// draw just beyond the horizon
			int new_col = ((cam.x + SCREEN_WIDTH) >> 3) + 1;
			if (new_col > horizon) {
				draw_col(bg0_map, new_col);
				horizon = new_col;
			}
		}
		
		// things we need to do when the player moves vertically
		player_state_t new_state = guy.state;
		
		if ((guy.state == FALL) &&
		    (abs((guy.s.height - SPRITE_HEIGHT(guy.s)) - ground) <= GROUND_THRESH)) {
			if (ROLL_THRESH > guy.vy) {
				new_state = ROLL;
			} else {
				new_state = RUN;
			}
			guy.vy = 0;
			guy.ay = 0;
			guy.s.height = ground + SPRITE_HEIGHT(guy.s);
			DEBUGFMT("snapping to ground: %d", ground);
		}

		// update player state
		switch (guy.state) {
		case RUN:
		case ROLL:  // should we start falling
			if (guy.s.height - SPRITE_HEIGHT(guy.s) > ground) {
				new_state = FALL;
				guy.ay = GRAV;
			}
			if (jump_requested) {
				new_state = JUMP;
				guy.vy = JUMP_START_VEL;
				guy.ay = GRAV;
			}
			break;
		case JUMP:
			if (guy.vy < 0) { new_state = FALL; }
			if (ascent_ended) { guy.vy = MIN(guy.vy, JUMP_END_VEL); }
			break;
		case FALL: // did we hit the ground
			if (guy.s.height < 0) {
				new_state = DEAD;
				guy.vy = 0;
				guy.ay = 0;
				DEBUG("died! resetting");
				goto reset;
			}
			break;
		default:
			break;
		}

		// get next frame of animation
		frame_count++;
		if (new_state == guy.state) {
			if (frame_count % 2 == 0) {
				guy.anim.frame++;
				guy.anim.frame %= guy.anim.len;
				if ((guy.state == ROLL) && (guy.anim.frame == 0)) {
					new_state = RUN;
					guy.anim = ANIM_RUN;
				}
			}
		} else {
			switch (new_state) {
			case RUN:
				guy.anim = ANIM_RUN; break;
			case JUMP:
				guy.anim = ANIM_RUN; break;
			case FALL:
				guy.anim = ANIM_FALL; break;
			case ROLL:
				guy.anim = ANIM_ROLL; break;
			default: break;
			}
			guy.state = new_state;
		}

		// move player
		guy.vy += guy.ay;
		guy.s.height += (int)guy.vy;
		
		// apply camera y adjustments
		if (((int)guy.vy != 0) && // without this check this hits the cpu
		    ((SCREEN_Y(guy.s, cam) > LOWER_SLACK) ||
		     (SCREEN_Y(guy.s, cam) < UPPER_SLACK))) { 
			cam.y -= (int)guy.vy;
			cam.y = CLAMP(cam.y, 0, PIXEL(WORLD_HEIGHT) - SCREEN_HEIGHT);
		}
		
		// apply player attributes
		guy.s.tile = guy.anim.start + 2*guy.anim.frame;
		place_sprite(&guy.s, &cam);

		// apply crate attributes
		for (int i = 0; i < MAX_CRATES; i++) {
			if (crates[i].valid) {
				if (crates[i].ay) {
					crates[i].vy += crates[i].ay;
					crates[i].s.height += (int)crates[i].vy;
					
					crates[i].s.x += (int)crates[i].vx;
				}
				bool present_before = (crates[i].s.obj != NULL);
				place_sprite(&crates[i].s, &cam);
				if (!crates[i].s.obj && present_before) {
					DEBUG("destructing a crate");
					crates[i].valid = false;
					num_crates--;
				}
			}
		}

		// CORRECTION DU SCORE : Affiche les mètres parcourus, pas le nombre de caisses
		set_score(cam.x >> 3);

		// update bg regs
		REG_BG0HOFS = cam.x;
		REG_BG0VOFS = cam.y;
		REG_BG1HOFS = cam.x * BG1_SCROLL_RATE;
		REG_BG1VOFS = cam.y * BG1_SCROLL_RATE + BG1_YOFFS;
		REG_BG2HOFS = cam.x * BG2_SCROLL_RATE;
		REG_BG2VOFS = cam.y * BG2_SCROLL_RATE + BG2_YOFFS;
		
		// update oam
		copy_objs();
	}

	return 0;
}

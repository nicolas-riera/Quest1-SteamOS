// What driver_quest1 (controllers.cpp) tells q1handviz (src/q1handviz) about the tracked hands, in a small file on
// the tmpfs: written in place by the driver at its update rate. Plain data, one writer.
#pragma once
#include <stdint.h>
#define Q1HANDVIZ_PATH "/run/q1track/handviz"
#define Q1HANDVIZ_MAGIC 0x5a495648u
struct q1handviz_hand {
	uint32_t tracked;    // the hand is the device now
	uint32_t palmFacing; // the palm faces the user
	uint32_t pinch;      // thumb and index pinched
	uint32_t pad;
	float rootRot[4];    // w x y z: the skeleton's root in the device's (pointer) frame
	float rootPos[3];
	float palmDev[3];    // out of the palm, skeleton frame
	float joints[24][3]; // Meta's joints, skeleton frame
};
struct q1handviz_shm {
	uint32_t magic;
	volatile uint32_t seq;
	int64_t updateNs;    // CLOCK_MONOTONIC
	struct q1handviz_hand hand[2];
};

// Every crypto call the owner waits for, timed on the chip that runs it.
//
// The KEF key derivation was 200,000 SHA-256 compressions in portable C for
// the whole life of the feature and nobody noticed, because it is one line of
// code. No desktop gate can see it: the simulator has no accelerator, so its
// numbers describe a laptop. A grep for slow primitives cannot see it either
// -- the primitive is inside libwally and the count is a runtime argument.
//
// A clock can. This runs at every non release boot and prints what each
// blocking operation actually cost on this board, so a derivation that goes
// back to software, or a new one that arrives already slow, shows up as a
// number on the console instead of as an owner saying the screen froze.
//
// KISS_BENCH_BUDGET_US is the longest a screen may sit dead without anything
// on it to say why. Over that prints SLOW, and SLOW means one of two things:
// make it faster, or move it off the UI task and put something on the glass.
#ifndef KISS_CRYPTOBENCH_H
#define KISS_CRYPTOBENCH_H

#define KISS_BENCH_BUDGET_US 250000

// No-op in release and on the desktop. Requires wally_init to have run.
void kiss_cryptobench_run(void);

#endif // KISS_CRYPTOBENCH_H

// SPDX-License-Identifier: BSD-3-Clause
#ifndef PIANO_GOODIX_EVENTS_H
#define PIANO_GOODIX_EVENTS_H

#include <stddef.h>
#include <stdint.h>

#define GOODIX_EVENT_BUFFER_BYTES 0xd134U

enum goodix_events_state {
	GOODIX_EVENTS_IDLE,
	GOODIX_EVENTS_WAIT_IRQ,
	GOODIX_EVENTS_DRAIN,
	GOODIX_EVENTS_FAILED,
};

/* Sensor metadata only. An interrupt or event mask is never a match result. */
struct goodix_sensor_event {
	uint32_t mask;
	uint32_t operation;
	uint32_t phase;
	int32_t application_status;
};

/* The caller serializes commands and owns the shared buffer and IRQ fd.
 * command returns transport errors separately from the TA's operation status.
 * This interface does not install signals, poll, spawn a thread or log secrets.
 */
struct goodix_events {
	int (*command)(void *context, uint32_t command, size_t bytes,
		       int32_t *application_status);
	void *context;
	uint8_t *buffer;
	size_t buffer_size;
	enum goodix_events_state state;
};

int goodix_events_start(struct goodix_events *events);

/* Call after consuming a UIO IRQ, then again while the result is 1.
 * A zero result completes that IRQ; only then may the caller re-enable it.
 * Negative results require cancel/close, not a synthetic authentication result.
 */
int goodix_events_read(struct goodix_events *events, struct goodix_sensor_event *event);
int goodix_events_cancel(struct goodix_events *events);

#endif

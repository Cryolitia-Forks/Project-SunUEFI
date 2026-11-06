// SPDX-License-Identifier: BSD-3-Clause
/* Milan event transport recovered from the factory GF3656ZSCB HAL.
 * See docs/devel/hardware/led-ir-fingerprint.md for function addresses.
 */
#include <errno.h>
#include <string.h>

#include "piano-goodix-events.h"

static uint32_t read_word(const uint8_t *buffer, size_t offset)
{
	uint32_t value;

	memcpy(&value, buffer + offset, sizeof(value));
	return value;
}

static void write_word(uint8_t *buffer, size_t offset, uint32_t value)
{
	memcpy(buffer + offset, &value, sizeof(value));
}

static int idle(struct goodix_events *events)
{
	int32_t status = 0;

	memset(events->buffer, 0, events->buffer_size);
	/* Milan cancel at HAL 0x40188 sends GF_CMD_CANCEL (1009), not 1011. */
	int result = events->command(events->context, 1009, 0x70, &status);
	if (!result && status)
		result = -EREMOTEIO;
	events->state = result ? GOODIX_EVENTS_FAILED : GOODIX_EVENTS_IDLE;
	return result;
}

int goodix_events_start(struct goodix_events *events)
{
	if (!events->command || !events->buffer ||
	    events->buffer_size < GOODIX_EVENT_BUFFER_BYTES)
		return -EINVAL;
	if (events->state != GOODIX_EVENTS_IDLE)
		return -EBUSY;
	int result = idle(events);

	if (!result)
		events->state = GOODIX_EVENTS_WAIT_IRQ;
	return result;
}

int goodix_events_read(struct goodix_events *events, struct goodix_sensor_event *event)
{
	int32_t status = 0;

	if (events->state != GOODIX_EVENTS_WAIT_IRQ &&
	    events->state != GOODIX_EVENTS_DRAIN)
		return -EINVAL;
	memset(event, 0, sizeof(*event));
	if (events->state == GOODIX_EVENTS_WAIT_IRQ) {
		memset(events->buffer, 0, events->buffer_size);
		/* Factory Milan IRQ at 0x41390 starts phase 1, with a 512-byte
		 * result limit. The output HAT's authenticator_type is a big-endian
		 * HW_AUTH_FINGERPRINT value, not an enrollment authorization token.
		 */
		write_word(events->buffer, 0x98, 1);
		write_word(events->buffer, 0xa8, 512);
		write_word(events->buffer, 0x2c5, 0x02000000);
	}
	int result = events->command(events->context, 1016,
				     GOODIX_EVENT_BUFFER_BYTES, &status);
	if (result) {
		events->state = GOODIX_EVENTS_FAILED;
		return result;
	}
	event->mask = read_word(events->buffer, 0x6c);
	event->operation = read_word(events->buffer, 0x70);
	event->phase = read_word(events->buffer, 0x98);
	event->application_status = status;
	/* HAL 0x415a4..0x41610 preserves the complete packet while advancing
	 * phases 4 and 5. Reinitializing it here would lose the TA's state.
	 * Enrollment/matching phases need further processing and are not exposed
	 * as completed operations by this diagnostic interface.
	 */
	if (!status && (event->phase == 4 || event->phase == 5)) {
		events->state = GOODIX_EVENTS_DRAIN;
		return 1;
	}
	/* Phase 2 captures fingerprint frames; phase 8 is a navigation frame.
	 * The factory processes those buffers before advancing the handshake.
	 * A metadata watch cannot substitute an IRQ for that processing.
	 */
	if (event->phase == 2 || event->phase == 8) {
		events->state = GOODIX_EVENTS_FAILED;
		return -EOPNOTSUPP;
	}
	if (status) {
		events->state = GOODIX_EVENTS_FAILED;
		return -EREMOTEIO;
	}
	events->state = GOODIX_EVENTS_WAIT_IRQ;
	return 0;
}

int goodix_events_cancel(struct goodix_events *events)
{
	if (events->state == GOODIX_EVENTS_IDLE)
		return 0;
	return idle(events);
}

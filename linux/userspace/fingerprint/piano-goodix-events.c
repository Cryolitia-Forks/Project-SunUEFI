// SPDX-License-Identifier: BSD-3-Clause
/* Milan event transport recovered from the factory GF3656ZSCB HAL.
 * See docs/devel/hardware/led-ir-fingerprint.md for function addresses.
 */
#include <errno.h>
#include <string.h>

#include "piano-goodix-events.h"

#define GOODIX_FINGER_DOWN (1U << 1)
#define GOODIX_IMAGE_RESULT (1U << 7)
#define GOODIX_MATCH_RETRY 1064
#define GOODIX_NAV_RESULT_OFFSET 0x2f4U
#define GOODIX_NAV_RESULT_BYTES 0x5a20U

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
	int retry = events->state == GOODIX_EVENTS_RETRY;
	int navigation = 0;
	int can_begin_capture = events->state == GOODIX_EVENTS_WAIT_IRQ;

	if (events->state != GOODIX_EVENTS_WAIT_IRQ &&
	    events->state != GOODIX_EVENTS_DRAIN && !retry)
		return -EINVAL;
	memset(event, 0, sizeof(*event));
	if (events->state == GOODIX_EVENTS_WAIT_IRQ) {
		events->capture_state = GOODIX_CAPTURE_NONE;
		memset(events->buffer, 0, events->buffer_size);
		/* Factory Milan IRQ at 0x41390 starts phase 1, with a 512-byte
		 * result limit. The output HAT's authenticator_type is a big-endian
		 * HW_AUTH_FINGERPRINT value, not an enrollment authorization token.
		 */
		write_word(events->buffer, 0x98, 1);
		write_word(events->buffer, 0xa8, 512);
		write_word(events->buffer, 0x2c5, 0x02000000);
	} else if (retry) {
		/* HAL 0x41e18..0x41e28 stores a 64-bit 2, clearing the word
		 * after the phase as well. Preserve the rest of the returned packet.
		 */
		write_word(events->buffer, 0x98, 2);
		write_word(events->buffer, 0x9c, 0);
	} else if (events->capture_state == GOODIX_CAPTURE_PENDING) {
		events->capture_state = GOODIX_CAPTURE_ADVANCED;
	} else if (read_word(events->buffer, 0x98) == 8) {
		navigation = 1;
		/* HAL 0x4169c..0x416b8 consumes and clears the navigation result
		 * before the next exchange. The watcher does not emit navigation keys.
		 */
		memset(events->buffer + GOODIX_NAV_RESULT_OFFSET, 0,
		       GOODIX_NAV_RESULT_BYTES);
	} else {
		uint32_t phase = read_word(events->buffer, 0x98);

		can_begin_capture = phase == 4 || phase == 5;
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

	/* Only a successful initial/phase-4/5 exchange can enter ordinary capture.
	 * HAL 0x41450..0x41458 and 0x41614..0x41618 clear the phase on errors;
	 * a later retry response cannot turn those errors into a pending capture.
	 * An already pending ordinary exchange survives a nested retry.
	 */
	if (!status && can_begin_capture && event->phase == 2 &&
	    events->capture_state == GOODIX_CAPTURE_NONE &&
	    (event->mask & GOODIX_FINGER_DOWN))
		events->capture_state = GOODIX_CAPTURE_PENDING;

	/* The image-result handler enters the 1064 loop only for the ordinary
	 * fingerprint operations (HAL 0x41bc8..0x41dac). Later retries may omit
	 * image bit 7; HAL 0x41e30..0x41e48 still continues while status is 1064.
	 * Enrollment is deliberately excluded: it may require an extended packet.
	 */
	if (status == GOODIX_MATCH_RETRY && event->operation >= 1 &&
	    event->operation <= 4 && (retry || (event->mask & GOODIX_IMAGE_RESULT))) {
		events->state = GOODIX_EVENTS_RETRY;
		return 1;
	}
	if (status) {
		events->state = GOODIX_EVENTS_FAILED;
		return -EREMOTEIO;
	}
	/* HAL 0x42774..0x42834 completes image/FF authentication with UPDATE_CONFIG
	 * (1109) and AUTHENTICATE_FINISH (1086). FIDO takes 0x424a0 -> 0x427cc and
	 * still reaches AUTHENTICATE_FINISH at 0x42830. Until a real authentication
	 * caller owns those flows, cancel instead of declaring the IRQ drained or
	 * exposing group, template ID, HAT or FIDO result bytes.
	 */
	if ((event->mask & GOODIX_IMAGE_RESULT) &&
	    (event->operation == 1 || event->operation == 2 || event->operation == 4)) {
		events->state = GOODIX_EVENTS_FAILED;
		return -EOPNOTSUPP;
	}

	/* HAL 0x41728..0x41778 advances ordinary phase 2 once with the same packet;
	 * the matching algorithm runs in the signed TA, not in the Android HAL.
	 * The following result handler at 0x41a8c can retry, but does not repeat
	 * the ordinary phase-2 exchange merely because its phase is unchanged.
	 * Operation 0 may select the 0x10e9e0 enrollment packet at 0x41760.
	 * NAV (6) and stable-factor testing (47) use different branches.
	 */
	if ((event->phase == 2 && (event->operation == 0 ||
				 event->operation == 6 || event->operation == 47)) ||
	    (event->phase == 8 && event->operation != 6)) {
		events->state = GOODIX_EVENTS_FAILED;
		return -EOPNOTSUPP;
	}
	/* New capture/navigation branches require finger-down bit 1 at HAL
	 * 0x415e4/0x4164c. An ongoing navigation loop only tests its phase.
	 */
	if (events->capture_state == GOODIX_CAPTURE_PENDING ||
	    event->phase == 4 || event->phase == 5 ||
	    (event->phase == 8 && (navigation || (event->mask & GOODIX_FINGER_DOWN)))) {
		events->state = GOODIX_EVENTS_DRAIN;
		return 1;
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

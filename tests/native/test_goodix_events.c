// SPDX-License-Identifier: BSD-3-Clause
/* Exercise the real event code against the factory packet exchanges. No TEE,
 * UIO, firmware, user templates or biometric device is opened by this test.
 */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../linux/userspace/fingerprint/piano-goodix-events.h"

struct reply {
	uint32_t request_phase, mask, operation, phase;
	int32_t status;
	int transport_error;
};

struct exchange {
	uint8_t buffer[GOODIX_EVENT_BUFFER_BYTES + 1];
	const struct reply *replies;
	size_t count, index;
	int cancel_count;
	int32_t cancel_status;
};

static uint32_t word(const uint8_t *buffer, size_t offset)
{
	uint32_t value;

	memcpy(&value, buffer + offset, sizeof(value));
	return value;
}

static void put_word(uint8_t *buffer, size_t offset, uint32_t value)
{
	memcpy(buffer + offset, &value, sizeof(value));
}

static int command(void *context, uint32_t number, size_t bytes, int32_t *status)
{
	struct exchange *exchange = context;
	uint8_t *buffer = exchange->buffer;

	assert(buffer[GOODIX_EVENT_BUFFER_BYTES] == 0x6d);
	if (number == 1009) {
		assert(bytes == 0x70);
		for (size_t i = 0; i < GOODIX_EVENT_BUFFER_BYTES; ++i)
			assert(buffer[i] == 0);
		++exchange->cancel_count;
		*status = exchange->cancel_status;
		return 0;
	}
	assert(number == 1016);
	assert(bytes == GOODIX_EVENT_BUFFER_BYTES);
	assert(exchange->index < exchange->count);
	const struct reply *reply = &exchange->replies[exchange->index++];

	assert(word(buffer, 0x98) == reply->request_phase);
	if (reply->request_phase == 1) {
		assert(word(buffer, 0xa8) == 512);
		assert(word(buffer, 0x2c5) == 0x02000000);
		buffer[0x2f3] = 0x37;
		buffer[0x5d14] = 0x59;
	} else if (reply->request_phase == 8) {
		/* 0x4169c: clear exactly [0x2f4, 0x5d14), not the whole packet. */
		for (size_t i = 0x2f4; i < 0x5d14; ++i)
			assert(buffer[i] == 0);
		assert(buffer[0x2f3] == 0x37);
		assert(buffer[0x5d14] == 0x59);
	} else {
		assert(buffer[0x2f3] == 0x37);
		assert(buffer[0x5d14] == 0x59);
	}
	/* Every ordinary/continuation packet keeps the TA's opaque data. The
	 * watcher only extracts mask/operation/phase/status, never these bytes.
	 */
	if (reply->request_phase != 1)
		assert(word(buffer, 0x80) == 0xdecafbad);
	if (exchange->index > 1 &&
	    exchange->replies[exchange->index - 2].status == 1064)
		assert(word(buffer, 0x9c) == 0);
	if (reply->status == 1064)
		put_word(buffer, 0x9c, 0xffffffff);
	if (reply->phase == 8)
		memset(buffer + 0x2f4, 0xa5, 0x5a20);
	put_word(buffer, 0x80, 0xdecafbad);
	put_word(buffer, 0x6c, reply->mask);
	put_word(buffer, 0x70, reply->operation);
	put_word(buffer, 0x98, reply->phase);
	*status = reply->status;
	return reply->transport_error;
}

static void run_sequence(const struct reply *replies, const int *results,
			 size_t count)
{
	struct exchange exchange = { .replies = replies, .count = count };
	struct goodix_events events = {
		.command = command, .context = &exchange,
		.buffer = exchange.buffer, .buffer_size = GOODIX_EVENT_BUFFER_BYTES,
	};

	exchange.buffer[GOODIX_EVENT_BUFFER_BYTES] = 0x6d;
	assert(goodix_events_start(&events) == 0);
	assert(events.state == GOODIX_EVENTS_WAIT_IRQ);
	for (size_t i = 0; i < count; ++i) {
		struct goodix_sensor_event event;

		assert(goodix_events_read(&events, &event) == results[i]);
		assert(exchange.index == i + 1); /* One exchange per caller deadline check. */
		if (!replies[i].transport_error) {
			assert(event.mask == replies[i].mask);
			assert(event.operation == replies[i].operation);
			assert(event.phase == replies[i].phase);
			assert(event.application_status == replies[i].status);
		}
	}
	if (results[count - 1] < 0)
		assert(events.state == GOODIX_EVENTS_FAILED);
	else if (results[count - 1])
		assert(events.state == GOODIX_EVENTS_DRAIN || events.state == GOODIX_EVENTS_RETRY);
	else
		assert(events.state == GOODIX_EVENTS_WAIT_IRQ);
	assert(goodix_events_cancel(&events) == 0);
	assert(events.state == GOODIX_EVENTS_IDLE);
	assert(goodix_events_cancel(&events) == 0);
	assert(exchange.cancel_count == 2);
}

static void capture(void)
{
	/* 0x415a4/0x415f8: retain phase 4/5 packets; 0x4176c: advance phase 2
	 * once. An unchanged phase on its reply does not start another capture.
	 */
	const struct reply replies[] = {
		{ .request_phase = 1, .mask = 2, .operation = 5, .phase = 4 },
		{ .request_phase = 4, .mask = 2, .operation = 5, .phase = 5 },
		{ .request_phase = 5, .mask = 2, .operation = 5, .phase = 2 },
		{ .request_phase = 2, .mask = 0x80, .operation = 5, .phase = 2 },
		{ .request_phase = 1, .mask = 2, .operation = 5, .phase = 2 },
		{ .request_phase = 2, .mask = 0x80, .operation = 5, .phase = 0 },
	};
	const int results[] = { 1, 1, 1, 0, 1, 0 };

	run_sequence(replies, results, sizeof(replies) / sizeof(replies[0]));
}

static void retry(void)
{
	/* 0x41e24: u64 phase=2; keep retrying status 1064 even when image bit 7
	 * is absent. A failed initial IRQ must not start ordinary capture merely
	 * because its retry finishes with phase 2 and finger-down still set.
	 */
	const struct reply failed_initial[] = {
		{ .request_phase = 1, .mask = 0x82, .operation = 1, .phase = 2, .status = 1064 },
		{ .request_phase = 2, .operation = 1, .phase = 2, .status = 1064 },
		{ .request_phase = 2, .mask = 2, .operation = 1, .phase = 2 },
	};
	const struct reply after_capture[] = {
		{ .request_phase = 1, .mask = 2, .operation = 2, .phase = 2 },
		{ .request_phase = 2, .mask = 0x80, .operation = 2, .phase = 2, .status = 1064 },
		{ .request_phase = 2, .mask = 2, .operation = 2, .phase = 2 },
	};
	const int results[] = { 1, 1, 0 };

	run_sequence(failed_initial, results, 3);
	run_sequence(after_capture, results, 3);
	for (uint32_t phase = 4; phase <= 5; ++phase) {
		const struct reply failed_continuation[] = {
			{ .request_phase = 1, .mask = 2, .operation = 1, .phase = phase },
			{ .request_phase = phase, .mask = 0x82, .operation = 1, .phase = 2, .status = 1064 },
			{ .request_phase = 2, .mask = 2, .operation = 1, .phase = 2 },
		};

		run_sequence(failed_continuation, results, 3);
	}
}

static void navigation(void)
{
	const struct reply replies[] = {
		{ .request_phase = 1, .mask = 2, .operation = 6, .phase = 8 },
		{ .request_phase = 8, .operation = 6, .phase = 8 },
		{ .request_phase = 8, .operation = 6 },
	};
	const int results[] = { 1, 1, 0 };

	run_sequence(replies, results, 3);
}

static void errors(void)
{
	const struct reply replies[] = {
		{ .request_phase = 1, .operation = 0, .phase = 2 },
		{ .request_phase = 1, .operation = 6, .phase = 2 },
		{ .request_phase = 1, .operation = 47, .phase = 2 },
		{ .request_phase = 1, .operation = 1, .phase = 8 },
		{ .request_phase = 1, .operation = 1, .phase = 2, .status = 1064 },
		{ .request_phase = 1, .mask = 0x80, .operation = 1, .status = 1006 },
		{ .request_phase = 1, .mask = 0x80, .operation = 1 },
		{ .request_phase = 1, .mask = 0x82, .operation = 2, .phase = 2 },
		{ .request_phase = 1, .mask = 0x80, .operation = 4 },
		{ .request_phase = 1, .transport_error = -EIO },
	};
	const int results[] = {
		-EOPNOTSUPP, -EOPNOTSUPP, -EOPNOTSUPP, -EOPNOTSUPP,
		-EREMOTEIO, -EREMOTEIO, -EOPNOTSUPP, -EOPNOTSUPP, -EOPNOTSUPP, -EIO,
	};

	for (size_t i = 0; i < sizeof(replies) / sizeof(replies[0]); ++i)
		run_sequence(&replies[i], &results[i], 1);
	const struct reply pending[] = {
		{ .request_phase = 1, .mask = 0x80, .operation = 1, .phase = 2, .status = 1064 },
	};
	const int again[] = { 1 };

	run_sequence(pending, again, 1); /* Cancellation while retry is pending. */
	const struct reply authentication_result[] = {
		{ .request_phase = 1, .mask = 0x80, .operation = 1, .phase = 2, .status = 1064 },
		{ .request_phase = 2, .mask = 0x80, .operation = 1 },
	};
	const int authentication_results[] = { 1, -EOPNOTSUPP };

	/* A successful image after retry still requires unimplemented finish
	 * commands. It must cancel, not re-enable IRQ or report authentication.
	 */
	run_sequence(authentication_result, authentication_results, 2);
	const struct reply fido_result[] = {
		{ .request_phase = 1, .mask = 0x80, .operation = 4, .phase = 2, .status = 1064 },
		{ .request_phase = 2, .mask = 0x80, .operation = 4 },
	};

	run_sequence(fido_result, authentication_results, 2);
	const struct reply no_finger_down[] = {
		{ .request_phase = 1, .operation = 1, .phase = 2 },
		{ .request_phase = 1, .operation = 6, .phase = 8 },
	};
	const int done[] = { 0, 0 };

	run_sequence(no_finger_down, done, 2);
	struct exchange exchange = { .cancel_status = 1007 };
	struct goodix_events events = {
		.command = command, .context = &exchange,
		.buffer = exchange.buffer, .buffer_size = GOODIX_EVENT_BUFFER_BYTES,
	};

	exchange.buffer[GOODIX_EVENT_BUFFER_BYTES] = 0x6d;
	assert(goodix_events_start(&events) == -EREMOTEIO);
	assert(events.state == GOODIX_EVENTS_FAILED);
	assert(exchange.cancel_count == 1);
}

int main(int argc, char **argv)
{
	assert(argc == 2);
	if (!strcmp(argv[1], "capture"))
		capture();
	else if (!strcmp(argv[1], "retry"))
		retry();
	else if (!strcmp(argv[1], "navigation"))
		navigation();
	else {
		assert(!strcmp(argv[1], "errors"));
		errors();
	}
	puts("Goodix event packet checks passed; no hardware commands were sent.");
	return 0;
}

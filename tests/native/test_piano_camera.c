// SPDX-License-Identifier: MIT
/* Compile the actual derived daemon. Its device ioctls are supplied below;
 * frame delivery uses a local socket, never a camera or loopback device.
 */
#define main PianoCameraMain
#define ioctl camera_ioctl
#include PIANO_CAMERAD_SOURCE
#undef ioctl
#undef main

#include <assert.h>

static int output_fd, capture_fd = -1;
static unsigned int request_width = 320, request_height = 240, request_fps = 30, calls;
static bool queued_change;
static int sensor_test_fd = -1, programmed_vblank;
static unsigned int front_test_fps = 10;

int camera_ioctl(int fd, unsigned long request, ...)
{
	va_list args;
	void *argument;

	va_start(args, request);
	argument = va_arg(args, void *);
	va_end(args);
	if (fd == sensor_test_fd) {
		if (request == VIDIOC_SUBDEV_G_FRAME_INTERVAL) {
			struct v4l2_subdev_frame_interval *interval = argument;

			interval->interval = (struct v4l2_fract){ 1, front_test_fps };
		} else if (request == VIDIOC_SUBDEV_G_FMT) {
			struct v4l2_subdev_format *format = argument;

			format->format.width = 6528;
			format->format.height = 4896;
			format->format.code = MEDIA_BUS_FMT_SBGGR10_1X10;
		} else if (request == VIDIOC_G_CTRL) {
			struct v4l2_control *control = argument;

			assert(control->id == V4L2_CID_HBLANK);
			control->value = 256;
		} else if (request == VIDIOC_G_EXT_CTRLS) {
			struct v4l2_ext_controls *controls = argument;

			assert(controls->count == 1);
			assert(controls->controls[0].id == V4L2_CID_PIXEL_RATE);
			controls->controls[0].value64 = 558933333;
		} else if (request == VIDIOC_QUERY_EXT_CTRL) {
			struct v4l2_query_ext_ctrl *control = argument;

			assert(control->id == V4L2_CID_VBLANK);
			control->minimum = 114;
			control->maximum = 65535 - 2160;
		} else {
			struct v4l2_control *control = argument;

			assert(request == VIDIOC_S_CTRL && control->id == V4L2_CID_VBLANK);
			programmed_vblank = control->value;
		}
		return 0;
	}
	if (fd == output_fd && request == VIDIOC_DQEVENT) {
		if (queued_change) {
			struct v4l2_event *event = argument;

			*event = (struct v4l2_event){ .type = V4L2_EVENT_SOURCE_CHANGE };
			queued_change = false;
			return 0;
		}
		errno = EAGAIN;
		return -1;
	}
	if (fd == capture_fd && request == VIDIOC_STREAMOFF) {
		errno = EIO;
		return -1;
	}
	assert(fd == output_fd);
	if (request == VIDIOC_G_FMT) {
		struct v4l2_format *format = argument;

		assert(calls == 0 || calls == 4);
		format->fmt.pix = (struct v4l2_pix_format){
			.width = request_width, .height = request_height,
			.pixelformat = V4L2_PIX_FMT_NV12,
			.bytesperline = request_width,
			.sizeimage = request_width * request_height * 3 / 2,
		};
	} else if (request == VIDIOC_G_PARM || request == VIDIOC_S_PARM) {
		struct v4l2_streamparm *rate = argument;

		assert(calls == (request == VIDIOC_G_PARM ? 1U : 5U));
		if (request == VIDIOC_G_PARM)
			rate->parm.output.timeperframe = (struct v4l2_fract){ 1, request_fps };
		else {
			assert(rate->parm.output.timeperframe.numerator == 1);
			assert(rate->parm.output.timeperframe.denominator == request_fps);
		}
	} else if (request == VIDIOC_REQBUFS) {
		struct v4l2_requestbuffers *buffers = argument;

		assert(calls == 2);
		assert(buffers->type == V4L2_BUF_TYPE_VIDEO_OUTPUT);
		assert(buffers->count == 0);
	} else {
		struct v4l2_format *format = argument;

		assert(calls == 3 && request == VIDIOC_S_FMT);
		assert(format->type == V4L2_BUF_TYPE_VIDEO_OUTPUT);
		assert(format->fmt.pix.width == request_width);
		assert(format->fmt.pix.height == request_height);
	}
	++calls;
	return 0;
}

static void geometry(void)
{
	const unsigned int sizes[][2] = {
		{ 1920, 1440 }, { 1920, 1080 }, { 3840, 2160 },
		{ 1280, 720 }, { 640, 480 }, { 320, 240 }, { 16, 4 },
	};

	for (unsigned int camera = 0; camera < NUM_CAMS; camera++) {
		struct cam_cfg mode = cam_cfgs[camera];
		struct v4l2_rect compose, crop;

		for (unsigned int i = 0; i < sizeof(sizes) / sizeof(*sizes); i++) {
			mode.out_w = sizes[i][0];
			mode.out_h = sizes[i][1];
			bool valid = mode.out_w <= mode.max_out_w &&
				mode.out_h <= mode.max_out_h;

			assert((isp_geometry(&mode, &compose, &crop) == 0) == valid);
			if (!valid)
				continue;
			assert(compose.width <= mode.in_w && compose.height <= mode.in_h);
			assert(crop.width == mode.out_w && crop.height == mode.out_h);
			assert(crop.left >= 0 && crop.top >= 0);
			assert(crop.left + crop.width <= compose.width);
			assert(crop.top + crop.height <= compose.height);
			assert(!(crop.left & 1) && !(crop.top & 1));
			assert((uint64_t)compose.width * 16 >= mode.in_w);
			assert((uint64_t)compose.height * 16 >= mode.in_h);
			/* Rounding the common scale ratio costs at most two pixels. */
			assert(llabs((int64_t)compose.width * mode.in_h -
				     (int64_t)compose.height * mode.in_w) <=
			       2 * (mode.in_w + mode.in_h));
		}
		mode.out_w = mode.max_out_w;
		mode.out_h = mode.max_out_h;
		assert(isp_geometry(&mode, &compose, &crop) == 0);
		assert(compose.width == mode.in_w && crop.left == 0);
		mode.out_w--;
		assert(isp_geometry(&mode, &compose, &crop) < 0);
	}
}

static void reconfigure(int stop_error)
{
	int sockets[2];
	struct cam camera = {
		.mode = cam_cfgs[0], .vid_fd = -1, .sensor_fd = -1,
		.vfe_fd = -1, .vcm_fd = -1,
	};

	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
	output_fd = camera.loop_fd = sockets[0];
	camera.cfg = &camera.mode;
	if (stop_error) {
		capture_fd = camera.vid_fd = sockets[1];
		camera.streaming = true;
		camera.stage = malloc(16);
		assert(camera.stage);
		queued_change = true;
		loopback_event(&camera);
		assert(!quit && !exit_status);
		assert(calls == 0 && camera.streaming && camera.stage);
		assert(camera.mode.out_w == 1920 && camera.mode.out_h == 1440);
		free(camera.stage);
	} else {
		assert(loopback_change_format(&camera) == 0);
		assert(calls == 6 && !camera.streaming);
		assert(camera.mode.out_w == request_width);
		assert(camera.mode.out_h == request_height);
		assert(camera.mode.fps == request_fps);
		assert(camera.mode.in_w == (request_fps == 60 ? 3840U : 4080U));
		size_t luma = (size_t)request_width * request_height;
		size_t size = luma * 3 / 2;
		uint8_t *frame = malloc(size);

		assert(frame && recv(sockets[1], frame, size, MSG_WAITALL) == (ssize_t)size);
		for (size_t i = 0; i < size; i++)
			assert(frame[i] == (i < luma ? 0 : 128));
		free(frame);
	}
	close(sockets[0]);
	close(sockets[1]);
}

static void modes(void)
{
	struct cam_cfg mode = cam_cfgs[0];
	struct v4l2_rect compose, crop;

	assert(select_capture_mode(&mode, 3840, 2160, 60) == 0);
	assert(mode.in_w == 3840 && mode.in_h == 2160 && mode.fps == 60);
	assert(select_capture_mode(&mode, 4080, 3060, 60) < 0);
	assert(select_capture_mode(&mode, 8160, 6120, 30) < 0);
	assert(select_capture_mode(&mode, 8160, 6120, 10) == 0);
	assert(mode.in_w == 8160 && mode.in_h == 6120 && mode.fps == 10);
	assert(isp_geometry(&mode, &compose, &crop) == 0);
	assert(crop.width == 8160 && crop.height == 6120);
	mode = cam_cfgs[1];
	assert(select_capture_mode(&mode, 3840, 2160, 30) < 0);
	assert(select_capture_mode(&mode, 1920, 1080, 60) < 0);
	assert(select_capture_mode(&mode, 3264, 2448, 30) == 0);
	assert(select_capture_mode(&mode, 6528, 4896, 30) < 0);
	assert(select_capture_mode(&mode, 6528, 4896, 10) == 0);
	assert(mode.in_w == 6528 && mode.in_h == 4896 && mode.fps == 10);
	assert(isp_geometry(&mode, &compose, &crop) == 0);
	assert(crop.width == 6528 && crop.height == 4896);
}

static void frame_timing(void)
{
	struct cam camera = { .mode = cam_cfgs[0], .sensor_fd = 1234 };

	camera.cfg = &camera.mode;
	sensor_test_fd = camera.sensor_fd;
	assert(select_capture_mode(&camera.mode, 3840, 2160, 60) == 0);
	assert(sensor_frame_rate(&camera) == 0);
	assert(programmed_vblank == 115);
	camera.mode.fps = 120;
	assert(sensor_frame_rate(&camera) < 0);
}

static void front_frame_timing(void)
{
	struct cam camera = { .mode = cam_cfgs[1], .sensor_fd = 1234 };
	struct v4l2_query_ext_ctrl exposure = {
		.type = V4L2_CTRL_TYPE_INTEGER, .minimum = 4, .maximum = 2539, .step = 1,
	};
	struct exposure_timing timing;

	camera.cfg = &camera.mode;
	sensor_test_fd = camera.sensor_fd;
	assert(select_capture_mode(&camera.mode, 6528, 4896, 10) == 0);
	assert(sensor_frame_rate(&camera) == 0);
	assert(exposure_timing(&camera, &exposure, &timing) == 0);
	assert(timing.line_ns_num == 1000000000 && timing.line_ns_den == 25510);
	assert(exposure_ns(&timing, 1) == 39200);
	camera.mode.fps = 30;
	assert(sensor_frame_rate(&camera) < 0);
}

int main(int argc, char **argv)
{
	assert(argc == 2);
	if (!strcmp(argv[1], "geometry"))
		geometry();
	else if (!strcmp(argv[1], "modes"))
		modes();
	else if (!strcmp(argv[1], "timing"))
		frame_timing();
	else if (!strcmp(argv[1], "front-timing"))
		front_frame_timing();
	else if (!strcmp(argv[1], "reconfigure") || !strcmp(argv[1], "reconfigure60")) {
		request_fps = !strcmp(argv[1], "reconfigure60") ? 60 : 30;
		reconfigure(0);
	}
	else {
		assert(!strcmp(argv[1], "stop-error"));
		reconfigure(1);
	}
	return 0;
}

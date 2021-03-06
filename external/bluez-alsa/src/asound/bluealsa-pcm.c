/*
 * bluealsa-pcm.c
 * Copyright (c) 2016-2017 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>

#include <alsa/asoundlib.h>
#include <alsa/pcm_external.h>

#include "shared/ctl-client.h"
#include "shared/ctl-proto.h"
#include "shared/log.h"


/* Helper macro for obtaining the size of a static array. */
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))


struct bluealsa_pcm {
	snd_pcm_ioplug_t io;

	/* bluealsa socket */
	int fd;

	/* event file descriptor */
	int event_fd;

	/* requested transport */
	struct msg_transport *transport;
	struct libshm_data *shm;

	/* virtual hardware - ring buffer */
	snd_pcm_uframes_t io_ptr;
	pthread_t io_thread;
	bool io_started;

	/* communication and encoding/decoding delay */
	snd_pcm_sframes_t delay;
	/* user provided extra delay component */
	snd_pcm_sframes_t delay_ex;

	/* ALSA operates on frames, we on bytes */
	size_t frame_size;

};


/**
 * IO thread, which facilitates ring buffer. */
static void *io_thread(void *arg) {
	snd_pcm_ioplug_t *io = (snd_pcm_ioplug_t *)arg;

	struct bluealsa_pcm *pcm = io->private_data;
	const snd_pcm_channel_area_t *areas = snd_pcm_ioplug_mmap_areas(io);
	int16_t silence = snd_pcm_format_silence_16(io->format);

	debug("Starting IO loop");
	for (;;) {

		snd_pcm_uframes_t io_ptr = pcm->io_ptr;
		snd_pcm_uframes_t io_buffer_size = io->buffer_size;
		snd_pcm_uframes_t frames = io->period_size;
		char *buffer = areas->addr + (areas->first + areas->step * io_ptr) / 8;
		char *head = buffer;
		ssize_t ret;
		size_t len;

		/* If the leftover in the buffer is less than a whole period sizes,
		 * adjust the number of frames which should be transfered. It has
		 * turned out, that the buffer might contain fractional number of
		 * periods - it could be an ALSA bug, though, it has to be handled. */
		if (io_buffer_size - io_ptr < frames)
			frames = io_buffer_size - io_ptr;

		/* IO operation size in bytes */
		len = frames * pcm->frame_size;

		if (io->stream == SND_PCM_STREAM_CAPTURE) {

			/* Read the whole period "atomically". This will assure, that frames
			 * are not fragmented, so the pointer can be correctly updated. */
			ret = libshm_read_all(pcm->shm, head, len);
			if (ret < 0)
				goto final;

		}
		else {

			ret = libshm_write_all(pcm->shm, head, len);
			if (ret < 0)
				goto final;

			/* Silence processed period, so if the underrun occurs,
			 * we will play silence instead of previous samples. */
			snd_pcm_uframes_t i = frames * io->channels;
			while (i--)
				((int16_t *)buffer)[i] = silence;

		}

		pcm->io_ptr = (io_ptr + frames) % io_buffer_size;
		eventfd_write(pcm->event_fd, 1);

	}

final:
	debug("Exiting IO thread");
	eventfd_write(pcm->event_fd, 0xDEAD0000);
	return NULL;
}

static int bluealsa_start(snd_pcm_ioplug_t *io) {
	struct bluealsa_pcm *pcm = io->private_data;
	struct sched_param param = { 0 };
	int policy;

	debug("Starting");

	/* If the IO thread is already started, skip thread creation. Otherwise,
	 * we might end up with a bunch of IO threads reading or writing to the
	 * same FIFO simultaneously. */
	if (pcm->io_started)
		return 0;

	/* initialize delay calculation */
	pcm->delay = 0;

	if (bluealsa_pause_transport(pcm->fd, pcm->transport, false) == -1) {
		debug("Couldn't start PCM: %s", strerror(errno));
		return -errno;
	}

	pcm->io_started = true;
	if ((errno = pthread_create(&pcm->io_thread, NULL, io_thread, io)) != 0) {
		debug("Couldn't create IO thread: %s", strerror(errno));
		pcm->io_started = false;
		return -errno;
	}

	pthread_setname_np(pcm->io_thread, "pcm-io");

	/* give the feeding thread priority over the consumer */
	param.sched_priority = 9;
	(void) pthread_setschedparam(pcm->io_thread, SCHED_RR, &param);

	return 0;
}

static int bluealsa_stop(snd_pcm_ioplug_t *io) {
	struct bluealsa_pcm *pcm = io->private_data;
	debug("Stopping");
	if (pcm->io_started) {
		pcm->io_started = false;
		pthread_cancel(pcm->io_thread);
		pthread_join(pcm->io_thread, NULL);
	}
	return 0;
}

static snd_pcm_sframes_t bluealsa_pointer(snd_pcm_ioplug_t *io) {
	struct bluealsa_pcm *pcm = io->private_data;
	return pcm->io_ptr;
}

static int bluealsa_close(snd_pcm_ioplug_t *io) {
	struct bluealsa_pcm *pcm = io->private_data;
	debug("Closing plugin");
	close(pcm->fd);
	close(pcm->event_fd);
	free(pcm->transport);
	free(pcm);
	return 0;
}

static int bluealsa_hw_params(snd_pcm_ioplug_t *io, snd_pcm_hw_params_t *params) {
	struct bluealsa_pcm *pcm = io->private_data;
	(void)params;
	debug("Initializing HW");

	pcm->frame_size = (snd_pcm_format_physical_width(io->format) * io->channels) / 8;

	if ((pcm->shm = bluealsa_open_transport(pcm->fd, pcm->transport)) == -1) {
		debug("Couldn't open PCM transport: %s", strerror(errno));
		return -errno;
	}

	/* Indicate that our PCM is ready for writing, even though is is not 100%
	 * true - IO thread is not running yet. Some weird implementations might
	 * require PCM to be writable before the snd_pcm_start() call. */
	if (io->stream == SND_PCM_STREAM_PLAYBACK)
		eventfd_write(pcm->event_fd, 1);

	debug("Selected HW buffer: %zd periods x %zd bytes %c= %zd bytes",
			io->buffer_size / io->period_size, pcm->frame_size * io->period_size,
			io->period_size * (io->buffer_size / io->period_size) == io->buffer_size ? '=' : '<',
			io->buffer_size * pcm->frame_size);

	return 0;
}

static int bluealsa_hw_free(snd_pcm_ioplug_t *io) {
	struct bluealsa_pcm *pcm = io->private_data;
	debug("Freeing HW");

	if (!pcm->shm)
		return -1;

	if (bluealsa_close_transport(pcm->fd, pcm->transport) == -1)
		debug("Couldn't close PCM FIFO: %s", strerror(errno));

	libshm_close(pcm->shm);

	return 0;
}

static int bluealsa_prepare(snd_pcm_ioplug_t *io) {
	struct bluealsa_pcm *pcm = io->private_data;

	/* if PCM shm transport is not opened, report it right away */
	if (!pcm->shm)
		return -ENODEV;

	/* initialize ring buffer */
	pcm->io_ptr = 0;

	debug("Prepared");
	return 0;
}

static int bluealsa_drain(snd_pcm_ioplug_t *io) {
	(void)io;
	debug("Draining");
	return 0;
}

static int bluealsa_pause(snd_pcm_ioplug_t *io, int enable) {
	struct bluealsa_pcm *pcm = io->private_data;

	if (bluealsa_pause_transport(pcm->fd, pcm->transport, enable) == -1)
		return -errno;

	/* Even though PCM transport is paused, our IO thread is still running. If
	 * the implementer relies on the PCM file descriptor readiness, we have to
	 * bump our internal event trigger. Otherwise, client might stuck forever
	 * in the poll/select system call. */
	eventfd_write(pcm->event_fd, 1);

	return 0;
}

static void bluealsa_dump(snd_pcm_ioplug_t *io, snd_output_t *out) {
	struct bluealsa_pcm *pcm = io->private_data;
	char addr[18];

	ba2str(&pcm->transport->addr, addr);
	snd_output_printf(out, "Bluetooth device: %s\n", addr);
	snd_output_printf(out, "Bluetooth profile: %d\n", pcm->transport->type);
	snd_output_printf(out, "Bluetooth codec: %d\n", pcm->transport->codec);
}

static int bluealsa_delay(snd_pcm_ioplug_t *io, snd_pcm_sframes_t *delayp) {
	struct bluealsa_pcm *pcm = io->private_data;

	/* Exact calculation of the PCM delay is very hard, if not impossible. For
	 * the sake of simplicity we will make few assumptions and approximations.
	 * In general, the delay is proportional to the number of bytes queued in
	 * the FIFO buffer, the time required to encode data, Bluetooth transfer
	 * latency and the time required by the device to decode and play audio. */

	static int counter = 0;
	snd_pcm_sframes_t delay = 0;
	unsigned int size;

	delay += io->stream == SND_PCM_STREAM_PLAYBACK ? libshm_bytes_outgoing(pcm->shm) : libshm_bytes_incoming(pcm->shm);

	/* data transfer (communication) and encoding/decoding */
	if (io->state == SND_PCM_STATE_RUNNING && io->stream == SND_PCM_STREAM_PLAYBACK &&
			(pcm->delay == 0 || ++counter % (io->rate / 10) == 0)) {

		int tmp;
		if ((tmp = bluealsa_get_transport_delay(pcm->fd, pcm->transport)) != -1) {
			pcm->delay = (io->rate / 100) * tmp / 100;
			debug("BlueALSA delay: %.1f ms (%ld frames)", (float)tmp / 10, pcm->delay);
		}

	}

	*delayp = delay + pcm->delay + pcm->delay_ex;
	return 0;
}

static int bluealsa_poll_descriptors_count(snd_pcm_ioplug_t *io) {
	(void)io;
	return 2;
}

static int bluealsa_poll_descriptors(snd_pcm_ioplug_t *io, struct pollfd *pfd,
		unsigned int space) {
	struct bluealsa_pcm *pcm = io->private_data;

	if (space != 2)
		return -EINVAL;

	/* PCM plug-in relies on the BlueALSA socket (critical signaling
	 * from the server) and our internal event file descriptor. */
	pfd[0].fd = pcm->event_fd;
	pfd[0].events = POLLIN;
	pfd[1].fd = pcm->fd;
	pfd[1].events = POLLIN;

	return 2;
}

static int bluealsa_poll_revents(snd_pcm_ioplug_t *io, struct pollfd *pfd,
		unsigned int nfds, unsigned short *revents) {
	struct bluealsa_pcm *pcm = io->private_data;

	if (nfds != 2)
		return -EINVAL;

	if (pfd[0].revents & POLLIN) {

		eventfd_t event;
		eventfd_read(pcm->event_fd, &event);

		if (event & 0xDEAD0000)
			goto fail;

		/* If the event was triggered prematurely, wait for another one. */
		if (!snd_pcm_avail_update(io->pcm))
			return *revents = 0;

		/* ALSA expects that the event will match stream direction, e.g.
		 * playback will not start if the event is for reading. */
		*revents = io->stream == SND_PCM_STREAM_CAPTURE ? POLLIN : POLLOUT;

	}
	else if (pfd[1].revents & POLLHUP)
		/* server closed connection */
		goto fail;
	else
		*revents = 0;

	return 0;

fail:
	io->state = SND_PCM_STATE_DISCONNECTED;
	*revents = POLLERR | POLLHUP;
	return -ENODEV;
}

static const snd_pcm_ioplug_callback_t bluealsa_callback = {
	.start = bluealsa_start,
	.stop = bluealsa_stop,
	.pointer = bluealsa_pointer,
	.close = bluealsa_close,
	.hw_params = bluealsa_hw_params,
	.hw_free = bluealsa_hw_free,
	.prepare = bluealsa_prepare,
	.drain = bluealsa_drain,
	.pause = bluealsa_pause,
	.dump = bluealsa_dump,
	.delay = bluealsa_delay,
	.poll_descriptors_count = bluealsa_poll_descriptors_count,
	.poll_descriptors = bluealsa_poll_descriptors,
	.poll_revents = bluealsa_poll_revents,
};

static enum pcm_type bluealsa_parse_profile(const char *profile) {

	if (profile == NULL)
		return PCM_TYPE_NULL;

	if (strcasecmp(profile, "a2dp") == 0)
		return PCM_TYPE_A2DP;
	else if (strcasecmp(profile, "sco") == 0)
		return PCM_TYPE_SCO;

	return PCM_TYPE_NULL;
}

static int bluealsa_set_hw_constraint(struct bluealsa_pcm *pcm) {
	snd_pcm_ioplug_t *io = &pcm->io;

	static const snd_pcm_access_t accesses[] = {
		SND_PCM_ACCESS_MMAP_INTERLEAVED,
		SND_PCM_ACCESS_RW_INTERLEAVED,
	};
	static const unsigned int formats[] = {
		SND_PCM_FORMAT_S16_LE,
	};

	int err;

	debug("Setting constraints");

	if ((err = snd_pcm_ioplug_set_param_list(io, SND_PCM_IOPLUG_HW_ACCESS,
					ARRAY_SIZE(accesses), accesses)) < 0)
		return err;

	if ((err = snd_pcm_ioplug_set_param_list(io, SND_PCM_IOPLUG_HW_FORMAT,
					ARRAY_SIZE(formats), formats)) < 0)
		return err;

	if ((err = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_PERIODS,
					2, 1024)) < 0)
		return err;

	if ((err = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_PERIOD_BYTES,
					128, 1024 * 4)) < 0)
		return err;

	if ((err = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_CHANNELS,
					pcm->transport->channels, pcm->transport->channels)) < 0)
		return err;

	if ((err = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_RATE,
					pcm->transport->sampling, pcm->transport->sampling)) < 0)
		return err;

	return 0;
}

SND_PCM_PLUGIN_DEFINE_FUNC(bluealsa) {
	(void)root;

	snd_config_iterator_t i, next;
	const char *interface = "hci0";
	const char *device = NULL;
	const char *profile = NULL;
	struct bluealsa_pcm *pcm;
	long delay = 0;
	int ret;

	snd_config_for_each(i, next, conf) {
		snd_config_t *n = snd_config_iterator_entry(i);

		const char *id;
		if (snd_config_get_id(n, &id) < 0)
			continue;

		if (strcmp(id, "comment") == 0 ||
				strcmp(id, "type") == 0 ||
				strcmp(id, "hint") == 0)
			continue;

		if (strcmp(id, "interface") == 0) {
			if (snd_config_get_string(n, &interface) < 0) {
				SNDERR("Invalid type for %s", id);
				return -EINVAL;
			}
			continue;
		}
		if (strcmp(id, "device") == 0) {
			if (snd_config_get_string(n, &device) < 0) {
				SNDERR("Invalid type for %s", id);
				return -EINVAL;
			}
			continue;
		}
		if (strcmp(id, "profile") == 0) {
			if (snd_config_get_string(n, &profile) < 0) {
				SNDERR("Invalid type for %s", id);
				return -EINVAL;
			}
			continue;
		}
		if (strcmp(id, "delay") == 0) {
			if (snd_config_get_integer(n, &delay) < 0) {
				SNDERR("Invalid type for %s", id);
				return -EINVAL;
			}
			continue;
		}

		SNDERR("Unknown field %s", id);
		return -EINVAL;
	}

	if ((pcm = calloc(1, sizeof(*pcm))) == NULL)
		return -ENOMEM;

	bdaddr_t addr;
	enum pcm_type type;

	if (device == NULL || str2ba(device, &addr) != 0) {
		SNDERR("Invalid BT device address: %s", device);
		ret = -errno;
		goto fail;
	}

	if ((type = bluealsa_parse_profile(profile)) == PCM_TYPE_NULL) {
		SNDERR("Invalid BT profile [a2dp, sco]: %s", profile);
		ret = -errno;
		goto fail;
	}

	pcm->fd = -1;
	pcm->event_fd = -1;
	pcm->delay_ex = delay;

	if ((pcm->fd = bluealsa_open(interface)) == -1) {
		SNDERR("BlueALSA connection failed: %s", strerror(errno));
		ret = -errno;
		goto fail;
	}

	if ((pcm->event_fd = eventfd(0, EFD_CLOEXEC)) == -1) {
		ret = -errno;
		goto fail;
	}

	enum pcm_stream _stream = stream == SND_PCM_STREAM_PLAYBACK ?
			PCM_STREAM_PLAYBACK : PCM_STREAM_CAPTURE;
	if ((pcm->transport = bluealsa_get_transport(pcm->fd, addr, type, _stream)) == NULL) {
		SNDERR("Couldn't get BlueALSA transport: %s", strerror(errno));
		ret = -errno;
		goto fail;
	}

	pcm->io.version = SND_PCM_IOPLUG_VERSION;
	pcm->io.name = "BlueALSA";
	pcm->io.flags = SND_PCM_IOPLUG_FLAG_LISTED;
	pcm->io.mmap_rw = 1;
	pcm->io.callback = &bluealsa_callback;
	pcm->io.private_data = pcm;
	pcm->transport->stream = _stream;

	if ((ret = snd_pcm_ioplug_create(&pcm->io, name, stream, mode)) < 0)
		goto fail;

	if ((ret = bluealsa_set_hw_constraint(pcm)) < 0) {
		snd_pcm_ioplug_delete(&pcm->io);
		goto fail;
	}

	*pcmp = pcm->io.pcm;
	return 0;

fail:
	if (pcm->fd != -1)
		close(pcm->fd);
	if (pcm->event_fd != -1)
		close(pcm->event_fd);
	free(pcm->transport);
	free(pcm);
	return ret;
}

SND_PCM_PLUGIN_SYMBOL(bluealsa);

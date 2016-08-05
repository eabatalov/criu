#include <netinet/tcp.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <linux/sockios.h>
#include "soccr.h"

#ifndef SIOCOUTQNSD
/* MAO - Define SIOCOUTQNSD ioctl if we don't have it */
#define SIOCOUTQNSD     0x894B
#endif

#ifndef TCP_REPAIR_WINDOW
#define TCP_REPAIR_WINDOW       29
#endif

struct tcp_repair_window {
	__u32   snd_wl1;
	__u32   snd_wnd;
	__u32   max_window;

	__u32   rcv_wnd;
	__u32   rcv_wup;
};

static void (*log)(unsigned int loglevel, const char *format, ...)
	__attribute__ ((__format__ (__printf__, 2, 3)));
static unsigned int log_level = 0;

void libsoccr_set_log(unsigned int level, void (*fn)(unsigned int level, const char *fmt, ...))
{
	log_level = level;
	log = fn;
}

#define loge(msg, ...) do { if (log && (log_level >= SOCCR_LOG_ERR)) log(SOCCR_LOG_ERR, msg, ##__VA_ARGS__); } while (0)
#define logd(msg, ...) do { if (log && (log_level >= SOCCR_LOG_DBG)) log(SOCCR_LOG_DBG, msg, ##__VA_ARGS__); } while (0)

static int tcp_repair_on(int fd)
{
	int ret, aux = 1;

	ret = setsockopt(fd, SOL_TCP, TCP_REPAIR, &aux, sizeof(aux));
	if (ret < 0)
		loge("Can't turn TCP repair mode ON");

	return ret;
}

static void tcp_repair_off(int fd)
{
	int aux = 0, ret;

	ret = setsockopt(fd, SOL_TCP, TCP_REPAIR, &aux, sizeof(aux));
	if (ret < 0)
		loge("Failed to turn off repair mode on socket: %m\n");
}

struct libsoccr_sk {
	int fd;
};

struct libsoccr_sk *libsoccr_pause(int fd)
{
	struct libsoccr_sk *ret;

	ret = malloc(sizeof(*ret));
	if (!ret)
		return NULL;

	if (tcp_repair_on(fd) < 0) {
		free(ret);
		return NULL;
	}

	ret->fd = fd;
	return ret;
}

void libsoccr_resume(struct libsoccr_sk *sk)
{
	tcp_repair_off(sk->fd);
	free(sk);
}

static int refresh_sk(struct libsoccr_sk *sk, struct libsoccr_sk_data *data, struct tcp_info *ti)
{
	int size;
	socklen_t olen = sizeof(*ti);

	if (getsockopt(sk->fd, SOL_TCP, TCP_INFO, ti, &olen) || olen != sizeof(*ti)) {
		loge("Failed to obtain TCP_INFO");
		return -1;
	}

	switch (ti->tcpi_state) {
	case TCP_ESTABLISHED:
	case TCP_CLOSE:
		break;
	default:
		loge("Unknown state %d\n", ti->tcpi_state);
		return -1;
	}

	if (ioctl(sk->fd, SIOCOUTQ, &size) == -1) {
		loge("Unable to get size of snd queue");
		return -1;
	}

	data->outq_len = size;

	if (ioctl(sk->fd, SIOCOUTQNSD, &size) == -1) {
		loge("Unable to get size of unsent data");
		return -1;
	}

	data->unsq_len = size;

	if (ioctl(sk->fd, SIOCINQ, &size) == -1) {
		loge("Unable to get size of recv queue");
		return -1;
	}

	data->inq_len = size;

	return 0;
}

static int get_stream_options(struct libsoccr_sk *sk, struct libsoccr_sk_data *data, struct tcp_info *ti)
{
	int ret;
	socklen_t auxl;
	int val;

	auxl = sizeof(data->mss_clamp);
	ret = getsockopt(sk->fd, SOL_TCP, TCP_MAXSEG, &data->mss_clamp, &auxl);
	if (ret < 0)
		goto err_sopt;

	data->opt_mask = ti->tcpi_options;
	if (ti->tcpi_options & TCPI_OPT_WSCALE) {
		data->snd_wscale = ti->tcpi_snd_wscale;
		data->rcv_wscale = ti->tcpi_rcv_wscale;
	}

	if (ti->tcpi_options & TCPI_OPT_TIMESTAMPS) {
		auxl = sizeof(val);
		ret = getsockopt(sk->fd, SOL_TCP, TCP_TIMESTAMP, &val, &auxl);
		if (ret < 0)
			goto err_sopt;

		data->timestamp = val;
	}

	return 0;

err_sopt:
	loge("\tsockopt failed");
	return -1;
}

static int get_window(struct libsoccr_sk *sk, struct libsoccr_sk_data *data)
{
	struct tcp_repair_window opt;
	socklen_t optlen = sizeof(opt);

	if (getsockopt(sk->fd, SOL_TCP,
			TCP_REPAIR_WINDOW, &opt, &optlen)) {
		/* Appeared since 4.8, but TCP_repair itself is since 3.11 */
		if (errno == ENOPROTOOPT)
			return 0;

		loge("Unable to get window properties");
		return -1;
	}

	data->flags |= SOCCR_FLAGS_WINDOW;
	data->snd_wl1		= opt.snd_wl1;
	data->snd_wnd		= opt.snd_wnd;
	data->max_window	= opt.max_window;
	data->rcv_wnd		= opt.rcv_wnd;
	data->rcv_wup		= opt.rcv_wup;

	return 0;
}

/*
 * This is how much data we've had in the initial libsoccr
 */
#define SOCR_DATA_MIN_SIZE	(16 * sizeof(__u32))

int libsoccr_get_sk_data(struct libsoccr_sk *sk, struct libsoccr_sk_data *data, unsigned data_size)
{
	struct tcp_info ti;

	if (!data || data_size < SOCR_DATA_MIN_SIZE)
		return -1;

	memset(data, 0, data_size);

	if (refresh_sk(sk, data, &ti))
		return -2;

	if (get_stream_options(sk, data, &ti))
		return -3;

	if (get_window(sk, data))
		return -4;

	return sizeof(struct libsoccr_sk_data);
}

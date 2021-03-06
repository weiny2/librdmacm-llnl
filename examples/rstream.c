/*
 * Copyright (c) 2011-2012 Intel Corporation.  All rights reserved.
 *
 * This software is available to you under the OpenIB.org BSD license
 * below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AWV
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <getopt.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <netdb.h>
#include <fcntl.h>
#include <unistd.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include <rdma/rdma_cma.h>
#include <rdma/rsocket.h>

struct test_size_param {
	int size;
	int option;
};

static struct test_size_param test_size[] = {
	{ 1 <<  6, 0 },
	{ 1 <<  7, 1 }, { (1 <<  7) + (1 <<  6), 1},
	{ 1 <<  8, 1 }, { (1 <<  8) + (1 <<  7), 1},
	{ 1 <<  9, 1 }, { (1 <<  9) + (1 <<  8), 1},
	{ 1 << 10, 1 }, { (1 << 10) + (1 <<  9), 1},
	{ 1 << 11, 1 }, { (1 << 11) + (1 << 10), 1},
	{ 1 << 12, 0 }, { (1 << 12) + (1 << 11), 1},
	{ 1 << 13, 1 }, { (1 << 13) + (1 << 12), 1},
	{ 1 << 14, 1 }, { (1 << 14) + (1 << 13), 1},
	{ 1 << 15, 1 }, { (1 << 15) + (1 << 14), 1},
	{ 1 << 16, 0 }, { (1 << 16) + (1 << 15), 1},
	{ 1 << 17, 1 }, { (1 << 17) + (1 << 16), 1},
	{ 1 << 18, 1 }, { (1 << 18) + (1 << 17), 1},
	{ 1 << 19, 1 }, { (1 << 19) + (1 << 18), 1},
	{ 1 << 20, 0 }, { (1 << 20) + (1 << 19), 1},
	{ 1 << 21, 1 }, { (1 << 21) + (1 << 20), 1},
	{ 1 << 22, 1 }, { (1 << 22) + (1 << 21), 1},
};
#define TEST_CNT (sizeof test_size / sizeof test_size[0])

enum rs_optimization {
	opt_mixed,
	opt_latency,
	opt_bandwidth
};

static int rs, lrs;
static int use_rs = 1;
static int use_async;
static int verify;
static int flags = MSG_DONTWAIT;
static int poll_timeout = 0;
static int custom;
static int use_fork;
static pid_t fork_pid;
static enum rs_optimization optimization;
static int size_option;
static int iterations = 1;
static int transfer_size = 1000;
static int transfer_count = 1000;
static int buffer_size;
static char test_name[10] = "custom";
static char *port = "7471";
static char *dst_addr;
static char *src_addr;
static struct timeval start, end;
static void *buf;

#define rs_socket(f,t,p)  use_rs ? rsocket(f,t,p)  : socket(f,t,p)
#define rs_bind(s,a,l)    use_rs ? rbind(s,a,l)    : bind(s,a,l)
#define rs_listen(s,b)    use_rs ? rlisten(s,b)    : listen(s,b)
#define rs_connect(s,a,l) use_rs ? rconnect(s,a,l) : connect(s,a,l)
#define rs_accept(s,a,l)  use_rs ? raccept(s,a,l)  : accept(s,a,l)
#define rs_shutdown(s,h)  use_rs ? rshutdown(s,h)  : shutdown(s,h)
#define rs_close(s)       use_rs ? rclose(s)       : close(s)
#define rs_recv(s,b,l,f)  use_rs ? rrecv(s,b,l,f)  : recv(s,b,l,f)
#define rs_send(s,b,l,f)  use_rs ? rsend(s,b,l,f)  : send(s,b,l,f)
#define rs_poll(f,n,t)	  use_rs ? rpoll(f,n,t)	   : poll(f,n,t)
#define rs_fcntl(s,c,p)   use_rs ? rfcntl(s,c,p)   : fcntl(s,c,p)
#define rs_setsockopt(s,l,n,v,ol) \
	use_rs ? rsetsockopt(s,l,n,v,ol) : setsockopt(s,l,n,v,ol)
#define rs_getsockopt(s,l,n,v,ol) \
	use_rs ? rgetsockopt(s,l,n,v,ol) : getsockopt(s,l,n,v,ol)

static void size_str(char *str, size_t ssize, long long size)
{
	long long base, fraction = 0;
	char mag;

	if (size >= (1 << 30)) {
		base = 1 << 30;
		mag = 'g';
	} else if (size >= (1 << 20)) {
		base = 1 << 20;
		mag = 'm';
	} else if (size >= (1 << 10)) {
		base = 1 << 10;
		mag = 'k';
	} else {
		base = 1;
		mag = '\0';
	}

	if (size / base < 10)
		fraction = (size % base) * 10 / base;
	if (fraction) {
		snprintf(str, ssize, "%lld.%lld%c", size / base, fraction, mag);
	} else {
		snprintf(str, ssize, "%lld%c", size / base, mag);
	}
}

static void cnt_str(char *str, size_t ssize, long long cnt)
{
	if (cnt >= 1000000000)
		snprintf(str, ssize, "%lldb", cnt / 1000000000);
	else if (cnt >= 1000000)
		snprintf(str, ssize, "%lldm", cnt / 1000000);
	else if (cnt >= 1000)
		snprintf(str, ssize, "%lldk", cnt / 1000);
	else
		snprintf(str, ssize, "%lld", cnt);
}

static void show_perf(void)
{
	char str[32];
	float usec;
	long long bytes;

	usec = (end.tv_sec - start.tv_sec) * 1000000 + (end.tv_usec - start.tv_usec);
	bytes = (long long) iterations * transfer_count * transfer_size * 2;

	/* name size transfers iterations bytes seconds Gb/sec usec/xfer */
	printf("%-10s", test_name);
	size_str(str, sizeof str, transfer_size);
	printf("%-8s", str);
	cnt_str(str, sizeof str, transfer_count);
	printf("%-8s", str);
	cnt_str(str, sizeof str, iterations);
	printf("%-8s", str);
	size_str(str, sizeof str, bytes);
	printf("%-8s", str);
	printf("%8.2fs%10.2f%11.2f\n",
		usec / 1000000., (bytes * 8) / (1000. * usec),
		(usec / iterations) / (transfer_count * 2));
}

static int size_to_count(int size)
{
	if (size >= 1000000)
		return 100;
	else if (size >= 100000)
		return 1000;
	else if (size >= 10000)
		return 10000;
	else if (size >= 1000)
		return 100000;
	else
		return 1000000;
}

static void init_latency_test(int size)
{
	char sstr[5];

	size_str(sstr, sizeof sstr, size);
	snprintf(test_name, sizeof test_name, "%s_lat", sstr);
	transfer_count = 1;
	transfer_size = size;
	iterations = size_to_count(transfer_size);
}

static void init_bandwidth_test(int size)
{
	char sstr[5];

	size_str(sstr, sizeof sstr, size);
	snprintf(test_name, sizeof test_name, "%s_bw", sstr);
	iterations = 1;
	transfer_size = size;
	transfer_count = size_to_count(transfer_size);
}

static void format_buf(void *buf, int size)
{
	uint8_t *array = buf;
	static uint8_t data;
	int i;

	for (i = 0; i < size; i++)
		array[i] = data++;
}

static int verify_buf(void *buf, int size)
{
	static long long total_bytes;
	uint8_t *array = buf;
	static uint8_t data;
	int i;

	for (i = 0; i < size; i++, total_bytes++) {
		if (array[i] != data++) {
			printf("data verification failed byte %lld\n", total_bytes);
			return -1;
		}
	}
	return 0;
}

static int do_poll(struct pollfd *fds)
{
	int ret;

	do {
		ret = rs_poll(fds, 1, poll_timeout);
	} while (!ret);

	return ret == 1 ? 0 : ret;
}

static int send_xfer(int size)
{
	struct pollfd fds;
	int offset, ret;

	if (verify)
		format_buf(buf, size);

	if (use_async) {
		fds.fd = rs;
		fds.events = POLLOUT;
	}

	for (offset = 0; offset < size; ) {
		if (use_async) {
			ret = do_poll(&fds);
			if (ret)
				return ret;
		}

		ret = rs_send(rs, buf + offset, size - offset, flags);
		if (ret > 0) {
			offset += ret;
		} else if (errno != EWOULDBLOCK && errno != EAGAIN) {
			perror("rsend");
			return ret;
		}
	}

	return 0;
}

static int recv_xfer(int size)
{
	struct pollfd fds;
	int offset, ret;

	if (use_async) {
		fds.fd = rs;
		fds.events = POLLIN;
	}

	for (offset = 0; offset < size; ) {
		if (use_async) {
			ret = do_poll(&fds);
			if (ret)
				return ret;
		}

		ret = rs_recv(rs, buf + offset, size - offset, flags);
		if (ret > 0) {
			offset += ret;
		} else if (errno != EWOULDBLOCK && errno != EAGAIN) {
			perror("rrecv");
			return ret;
		}
	}

	if (verify) {
		ret = verify_buf(buf, size);
		if (ret)
			return ret;
	}

	return 0;
}

static int sync_test(void)
{
	int ret;

	ret = dst_addr ? send_xfer(16) : recv_xfer(16);
	if (ret)
		return ret;

	return dst_addr ? recv_xfer(16) : send_xfer(16);
}

static int run_test(void)
{
	int ret, i, t;

	ret = sync_test();
	if (ret)
		goto out;

	gettimeofday(&start, NULL);
	for (i = 0; i < iterations; i++) {
		for (t = 0; t < transfer_count; t++) {
			ret = dst_addr ? send_xfer(transfer_size) :
					 recv_xfer(transfer_size);
			if (ret)
				goto out;
		}

		for (t = 0; t < transfer_count; t++) {
			ret = dst_addr ? recv_xfer(transfer_size) :
					 send_xfer(transfer_size);
			if (ret)
				goto out;
		}
	}
	gettimeofday(&end, NULL);
	show_perf();
	ret = 0;

out:
	return ret;
}

static void set_options(int rs)
{
	int val;

	if (buffer_size) {
		rs_setsockopt(rs, SOL_SOCKET, SO_SNDBUF, (void *) &buffer_size,
			      sizeof buffer_size);
		rs_setsockopt(rs, SOL_SOCKET, SO_RCVBUF, (void *) &buffer_size,
			      sizeof buffer_size);
	} else {
		val = 1 << 19;
		rs_setsockopt(rs, SOL_SOCKET, SO_SNDBUF, (void *) &val, sizeof val);
		rs_setsockopt(rs, SOL_SOCKET, SO_RCVBUF, (void *) &val, sizeof val);
	}

	val = 1;
	rs_setsockopt(rs, IPPROTO_TCP, TCP_NODELAY, (void *) &val, sizeof(val));

	if (flags & MSG_DONTWAIT)
		rs_fcntl(rs, F_SETFL, O_NONBLOCK);

	if (use_rs) {
		/* Inline size based on experimental data */
		if (optimization == opt_latency) {
			val = 384;
			rs_setsockopt(rs, SOL_RDMA, RDMA_INLINE, &val, sizeof val);
		} else if (optimization == opt_bandwidth) {
			val = 0;
			rs_setsockopt(rs, SOL_RDMA, RDMA_INLINE, &val, sizeof val);
		}
	}
}

static int server_listen(void)
{
	struct addrinfo hints, *res;
	int val, ret;

	memset(&hints, 0, sizeof hints);
	hints.ai_flags = RAI_PASSIVE;
 	ret = getaddrinfo(src_addr, port, &hints, &res);
	if (ret) {
		perror("getaddrinfo");
		return ret;
	}

	lrs = rs_socket(res->ai_family, res->ai_socktype, res->ai_protocol);
	if (lrs < 0) {
		perror("rsocket");
		ret = lrs;
		goto free;
	}

	val = 1;
	ret = rs_setsockopt(lrs, SOL_SOCKET, SO_REUSEADDR, &val, sizeof val);
	if (ret) {
		perror("rsetsockopt SO_REUSEADDR");
		goto close;
	}

	ret = rs_bind(lrs, res->ai_addr, res->ai_addrlen);
	if (ret) {
		perror("rbind");
		goto close;
	}

	ret = rs_listen(lrs, 1);
	if (ret)
		perror("rlisten");

close:
	if (ret)
		rs_close(lrs);
free:
	freeaddrinfo(res);
	return ret;
}

static int server_connect(void)
{
	struct pollfd fds;
	int ret;

	set_options(lrs);
	do {
		if (use_async) {
			fds.fd = lrs;
			fds.events = POLLIN;

			ret = do_poll(&fds);
			if (ret) {
				perror("rpoll");
				return ret;
			}
		}

		rs = rs_accept(lrs, NULL, 0);
	} while (rs < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
	if (rs < 0) {
		ret = rs;
		perror("raccept");
	}

	if (use_fork)
		fork_pid = fork();
	if (!fork_pid)
		set_options(rs);
	return ret;
}

static int client_connect(void)
{
	struct addrinfo *res;
	struct pollfd fds;
	int ret, err;
	socklen_t len;

 	ret = getaddrinfo(dst_addr, port, NULL, &res);
	if (ret) {
		perror("getaddrinfo");
		return ret;
	}

	rs = rs_socket(res->ai_family, res->ai_socktype, res->ai_protocol);
	if (rs < 0) {
		perror("rsocket");
		ret = rs;
		goto free;
	}

	set_options(rs);
	/* TODO: bind client to src_addr */

	ret = rs_connect(rs, res->ai_addr, res->ai_addrlen);
	if (ret && (errno != EINPROGRESS)) {
		perror("rconnect");
		goto close;
	}

	if (ret && (errno == EINPROGRESS)) {
		fds.fd = rs;
		fds.events = POLLOUT;
		ret = do_poll(&fds);
		if (ret)
			goto close;

		len = sizeof err;
		ret = rs_getsockopt(rs, SOL_SOCKET, SO_ERROR, &err, &len);
		if (ret)
			goto close;
		if (err) {
			ret = -1;
			errno = err;
			perror("async rconnect");
		}
	}

close:
	if (ret)
		rs_close(rs);
free:
	freeaddrinfo(res);
	return ret;
}

static int run(void)
{
	int i, ret = 0;

	buf = malloc(!custom ? test_size[TEST_CNT - 1].size : transfer_size);
	if (!buf) {
		perror("malloc");
		return -1;
	}

	if (!dst_addr) {
		ret = server_listen();
		if (ret)
			goto free;
	}

	printf("%-10s%-8s%-8s%-8s%-8s%8s %10s%13s\n",
	       "name", "bytes", "xfers", "iters", "total", "time", "Gb/sec", "usec/xfer");
	if (!custom) {
		optimization = opt_latency;
		ret = dst_addr ? client_connect() : server_connect();
		if (ret)
			goto free;

		for (i = 0; i < TEST_CNT && !fork_pid; i++) {
			if (test_size[i].option > size_option)
				continue;
			init_latency_test(test_size[i].size);
			run_test();
		}
		if (fork_pid)
			wait(NULL);
		else
			rs_shutdown(rs, SHUT_RDWR);
		rs_close(rs);

		if (!dst_addr && use_fork && !fork_pid)
			goto free;

		optimization = opt_bandwidth;
		ret = dst_addr ? client_connect() : server_connect();
		if (ret)
			goto free;
		for (i = 0; i < TEST_CNT && !fork_pid; i++) {
			if (test_size[i].option > size_option)
				continue;
			init_bandwidth_test(test_size[i].size);
			run_test();
		}
	} else {
		ret = dst_addr ? client_connect() : server_connect();
		if (ret)
			goto free;

		if (!fork_pid)
			ret = run_test();
	}

	if (fork_pid)
		wait(NULL);
	else
		rs_shutdown(rs, SHUT_RDWR);
	rs_close(rs);
free:
	free(buf);
	return ret;
}

static int set_test_opt(char *optarg)
{
	if (strlen(optarg) == 1) {
		switch (optarg[0]) {
		case 's':
			use_rs = 0;
			break;
		case 'a':
			use_async = 1;
			break;
		case 'b':
			flags = (flags & ~MSG_DONTWAIT) | MSG_WAITALL;
			break;
		case 'f':
			use_fork = 1;
			use_rs = 0;
			break;
		case 'n':
			flags |= MSG_DONTWAIT;
			break;
		case 'v':
			verify = 1;
			break;
		default:
			return -1;
		}
	} else {
		if (!strncasecmp("socket", optarg, 6)) {
			use_rs = 0;
		} else if (!strncasecmp("async", optarg, 5)) {
			use_async = 1;
		} else if (!strncasecmp("block", optarg, 5)) {
			flags = (flags & ~MSG_DONTWAIT) | MSG_WAITALL;
		} else if (!strncasecmp("nonblock", optarg, 8)) {
			flags |= MSG_DONTWAIT;
		} else if (!strncasecmp("verify", optarg, 6)) {
			verify = 1;
		} else if (!strncasecmp("fork", optarg, 4)) {
			use_fork = 1;
			use_rs = 0;
		} else {
			return -1;
		}
	}
	return 0;
}

int main(int argc, char **argv)
{
	int op, ret;

	while ((op = getopt(argc, argv, "s:b:B:I:C:S:p:T:")) != -1) {
		switch (op) {
		case 's':
			dst_addr = optarg;
			break;
		case 'b':
			src_addr = optarg;
			break;
		case 'B':
			buffer_size = atoi(optarg);
			break;
		case 'I':
			custom = 1;
			iterations = atoi(optarg);
			break;
		case 'C':
			custom = 1;
			transfer_count = atoi(optarg);
			break;
		case 'S':
			if (!strncasecmp("all", optarg, 3)) {
				size_option = 1;
			} else {
				custom = 1;
				transfer_size = atoi(optarg);
			}
			break;
		case 'p':
			port = optarg;
			break;
		case 'T':
			if (!set_test_opt(optarg))
				break;
			/* invalid option - fall through */
		default:
			printf("usage: %s\n", argv[0]);
			printf("\t[-s server_address]\n");
			printf("\t[-b bind_address]\n");
			printf("\t[-B buffer_size]\n");
			printf("\t[-I iterations]\n");
			printf("\t[-C transfer_count]\n");
			printf("\t[-S transfer_size or all]\n");
			printf("\t[-p port_number]\n");
			printf("\t[-T test_option]\n");
			printf("\t    s|sockets - use standard tcp/ip sockets\n");
			printf("\t    a|async - asynchronous operation (use poll)\n");
			printf("\t    b|blocking - use blocking calls\n");
			printf("\t    f|fork - fork server processing\n");
			printf("\t    n|nonblocking - use nonblocking calls\n");
			printf("\t    v|verify - verify data\n");
			exit(1);
		}
	}

	if (!(flags & MSG_DONTWAIT))
		poll_timeout = -1;

	ret = run();
	return ret;
}

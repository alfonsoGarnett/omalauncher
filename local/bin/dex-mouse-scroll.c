#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
#include <poll.h>
#include <pty.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <wayland-client.h>
#include "/usr/local/include/protocols/wlr-virtual-pointer-unstable-v1-client-protocol.h"
#include "/usr/local/include/protocols/wlr-virtual-pointer-unstable-v1-protocol.c"

static struct wl_seat *seat = NULL;
static struct zwlr_virtual_pointer_manager_v1 *vp_mgr = NULL;
static volatile sig_atomic_t g_running = 1;
static pid_t g_child_pid = -1;

static void sig_handler(int sig) {
	g_running = 0;
	if (g_child_pid > 0) {
		kill(g_child_pid, SIGTERM);
	}
}

static void registry_handle_global(void *data, struct wl_registry *registry,
		uint32_t name, const char *interface, uint32_t version) {
	if (strcmp(interface, wl_seat_interface.name) == 0) {
		seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
	} else if (strcmp(interface, zwlr_virtual_pointer_manager_v1_interface.name) == 0) {
		vp_mgr = wl_registry_bind(registry, name,
			&zwlr_virtual_pointer_manager_v1_interface, 2);
	}
}

static void registry_handle_global_remove(void *data, struct wl_registry *registry,
		uint32_t name) {}

static const struct wl_registry_listener registry_listener = {
	.global = registry_handle_global,
	.global_remove = registry_handle_global_remove,
};

static uint32_t get_time_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void reap_child(void) {
	if (g_child_pid > 0) {
		kill(g_child_pid, SIGTERM);
		usleep(50000);
		kill(g_child_pid, SIGKILL);
		waitpid(g_child_pid, NULL, WNOHANG);
		g_child_pid = -1;
	}
}

int main(int argc, char **argv) {
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sig_handler;
	sa.sa_flags = SA_RESTART;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	signal(SIGHUP, SIG_IGN);
	signal(SIGPIPE, SIG_IGN);

	system("pkill -f 'adb.*shell getevent' 2>/dev/null || true");

	const char *wdisp = getenv("WAYLAND_DISPLAY");
	if (!wdisp || strlen(wdisp) == 0) {
		setenv("WAYLAND_DISPLAY", "wayland-1", 1);
		wdisp = "wayland-1";
	}

	struct wl_display *display = NULL;
	for (int retries = 0; retries < 15 && g_running; retries++) {
		display = wl_display_connect(wdisp);
		if (display) break;
		usleep(500000);
	}

	if (!display) {
		fprintf(stderr, "[dex-mouse] Failed to connect to Wayland display '%s'\n", wdisp);
		return 1;
	}

	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);

	if (!seat || !vp_mgr) {
		fprintf(stderr, "[dex-mouse] Wayland compositor lacks seat or pointer_manager\n");
		wl_display_disconnect(display);
		return 1;
	}

	struct zwlr_virtual_pointer_v1 *vp =
		zwlr_virtual_pointer_manager_v1_create_virtual_pointer(vp_mgr, seat);
	wl_display_roundtrip(display);

	printf("[dex-mouse] Virtual pointer created on %s. Tracking motion, buttons and scroll...\n", wdisp);
	fflush(stdout);

	int wl_fd = wl_display_get_fd(display);
	double pending_dx = 0.0;
	double pending_dy = 0.0;
	int need_motion = 0;

	while (g_running) {
		int master_fd = -1;
		pid_t pid = forkpty(&master_fd, NULL, NULL, NULL);
		if (pid < 0) {
			perror("forkpty");
			sleep(2);
			continue;
		}

		if (pid == 0) {
			execlp("adb", "adb", "-s", "127.0.0.1:5556", "shell", "getevent", NULL);
			perror("execlp adb");
			_exit(127);
		}

		g_child_pid = pid;
		int flags = fcntl(master_fd, F_GETFL, 0);
		fcntl(master_fd, F_SETFL, flags | O_NONBLOCK);

		char stream_buf[4096];
		size_t stream_buf_len = 0;

		while (g_running) {
			while (wl_display_prepare_read(display) != 0) {
				wl_display_dispatch_pending(display);
			}
			wl_display_flush(display);

			struct pollfd pfds[2];
			pfds[0].fd = master_fd;
			pfds[0].events = POLLIN;
			pfds[1].fd = wl_fd;
			pfds[1].events = POLLIN;

			int ret = poll(pfds, 2, 5000);
			if (ret < 0) {
				wl_display_cancel_read(display);
				if (errno == EINTR) continue;
				break;
			}

			if (pfds[1].revents & POLLIN) {
				wl_display_read_events(display);
				wl_display_dispatch_pending(display);
			} else {
				wl_display_cancel_read(display);
			}

			if (pfds[0].revents & (POLLERR | POLLHUP)) {
				fprintf(stderr, "[dex-mouse] getevent pty hung up\n");
				break;
			}

			if (pfds[0].revents & POLLIN) {
				ssize_t n = read(master_fd, stream_buf + stream_buf_len,
					sizeof(stream_buf) - 1 - stream_buf_len);
				if (n <= 0) {
					if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
						continue;
					}
					fprintf(stderr, "[dex-mouse] read error or EOF from getevent\n");
					break;
				}
				stream_buf_len += n;
				stream_buf[stream_buf_len] = '\0';

				char *line_start = stream_buf;
				char *nl;
				while ((nl = strchr(line_start, '\n')) != NULL) {
					*nl = '\0';
					char dev[64];
					unsigned int type = 0, code = 0;
					unsigned int raw_val = 0;

					if (sscanf(line_start, "%63[^:]: %x %x %x", dev, &type, &code, &raw_val) == 4) {
						int32_t val;
						if (raw_val & 0x80000000) {
							val = (int32_t)raw_val;
						} else if ((raw_val & 0x8000) && raw_val <= 0xffff) {
							val = (int16_t)raw_val;
						} else {
							val = (int32_t)raw_val;
						}

						uint32_t event_time = get_time_ms();

						// type 2: EV_REL
						if (type == 2) {
							if (code == 0) { // REL_X
								pending_dx += (double)val;
								need_motion = 1;
							} else if (code == 1) { // REL_Y
								pending_dy += (double)val;
								need_motion = 1;
							} else if (code == 8) { // REL_WHEEL
								int32_t discrete = -val;
								double delta = (double)discrete * 15.0;

								zwlr_virtual_pointer_v1_axis_source(vp, WL_POINTER_AXIS_SOURCE_WHEEL);
								zwlr_virtual_pointer_v1_axis_discrete(vp, event_time,
									WL_POINTER_AXIS_VERTICAL_SCROLL,
									wl_fixed_from_double(delta),
									discrete);
								zwlr_virtual_pointer_v1_frame(vp);
								wl_display_flush(display);
							} else if (code == 6) { // REL_HWHEEL
								int32_t discrete = val;
								double delta = (double)discrete * 15.0;

								zwlr_virtual_pointer_v1_axis_source(vp, WL_POINTER_AXIS_SOURCE_WHEEL);
								zwlr_virtual_pointer_v1_axis_discrete(vp, event_time,
									WL_POINTER_AXIS_HORIZONTAL_SCROLL,
									wl_fixed_from_double(delta),
									discrete);
								zwlr_virtual_pointer_v1_frame(vp);
								wl_display_flush(display);
							}
						}
						// type 0: EV_SYN (flush motion)
						else if (type == 0 && code == 0) {
							if (need_motion && (pending_dx != 0.0 || pending_dy != 0.0)) {
								zwlr_virtual_pointer_v1_motion(vp, event_time,
									wl_fixed_from_double(pending_dx),
									wl_fixed_from_double(pending_dy));
								zwlr_virtual_pointer_v1_frame(vp);
								wl_display_flush(display);
								pending_dx = 0.0;
								pending_dy = 0.0;
								need_motion = 0;
							}
						}
						// type 1: EV_KEY (Mouse buttons)
						else if (type == 1) {
							if (code >= 0x110 && code <= 0x114) {
								uint32_t state = (raw_val != 0) ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED;
								zwlr_virtual_pointer_v1_button(vp, event_time, code, state);
								zwlr_virtual_pointer_v1_frame(vp);
								wl_display_flush(display);
							}
						}
					}
					line_start = nl + 1;
				}

				size_t parsed_len = line_start - stream_buf;
				if (parsed_len < stream_buf_len) {
					memmove(stream_buf, line_start, stream_buf_len - parsed_len);
					stream_buf_len -= parsed_len;
					stream_buf[stream_buf_len] = '\0';
				} else {
					stream_buf_len = 0;
					stream_buf[0] = '\0';
				}
			}
		}

		close(master_fd);
		reap_child();

		if (g_running) {
			fprintf(stderr, "[dex-mouse] getevent stream closed. Reconnecting in 2s...\n");
			sleep(2);
		}
	}

	reap_child();
	zwlr_virtual_pointer_v1_destroy(vp);
	wl_display_disconnect(display);
	return 0;
}

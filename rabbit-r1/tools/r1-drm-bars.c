// r1-drm-bars [seconds]: show colour bars on the first connected connector of the "mediatek" DRM card.
// Static, libdrm-free, for testing the r1 panel over adb. Bars top to bottom: white, red, green, blue.
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>

#define U64(p) ((uint64_t)(uintptr_t)(p))

static int find_card(void)
{
	for (int i = 0; i < 8; i++) {
		char path[32], name[64] = {0};
		snprintf(path, sizeof(path), "/dev/dri/card%d", i);
		int fd = open(path, O_RDWR | O_CLOEXEC);
		if (fd < 0)
			continue;
		struct drm_version v = {.name_len = sizeof(name) - 1, .name = name};
		if (!ioctl(fd, DRM_IOCTL_VERSION, &v) && !strcmp(name, "mediatek")) {
			printf("using %s\n", path);
			return fd;
		}
		close(fd);
	}
	return -1;
}

int main(int argc, char **argv)
{
	int secs = argc > 1 ? atoi(argv[1]) : 30;
	int fd = find_card();
	if (fd < 0) {
		fprintf(stderr, "no mediatek card\n");
		return 1;
	}
	if (ioctl(fd, DRM_IOCTL_SET_MASTER, 0))
		perror("SET_MASTER (continuing)");

	struct drm_mode_card_res res = {0};
	if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res)) { perror("GETRESOURCES"); return 1; }
	uint32_t crtcs[res.count_crtcs ? res.count_crtcs : 1], conns[res.count_connectors ? res.count_connectors : 1];
	uint32_t encs[res.count_encoders ? res.count_encoders : 1], fbs[res.count_fbs ? res.count_fbs : 1];
	res.crtc_id_ptr = U64(crtcs); res.connector_id_ptr = U64(conns);
	res.encoder_id_ptr = U64(encs); res.fb_id_ptr = U64(fbs);
	if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res)) { perror("GETRESOURCES"); return 1; }
	printf("crtcs=%u connectors=%u encoders=%u\n", res.count_crtcs, res.count_connectors, res.count_encoders);

	for (uint32_t c = 0; c < res.count_connectors; c++) {
		struct drm_mode_get_connector conn = {.connector_id = conns[c]};
		if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn)) { perror("GETCONNECTOR"); continue; }
		struct drm_mode_modeinfo modes[conn.count_modes ? conn.count_modes : 1];
		uint32_t cencs[conn.count_encoders ? conn.count_encoders : 1];
		conn.modes_ptr = U64(modes); conn.encoders_ptr = U64(cencs);
		conn.count_props = 0; conn.props_ptr = 0; conn.prop_values_ptr = 0;
		if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn)) { perror("GETCONNECTOR"); continue; }
		printf("connector %u type %u status %u modes %u\n", conn.connector_id, conn.connector_type,
		       conn.connection, conn.count_modes);
		if (conn.connection != 1 || !conn.count_modes || !conn.count_encoders)
			continue;

		struct drm_mode_modeinfo *m = &modes[0];
		printf("mode %s %ux%u@%u clock %u\n", m->name, m->hdisplay, m->vdisplay, m->vrefresh, m->clock);
		struct drm_mode_get_encoder enc = {.encoder_id = conn.encoder_id ? conn.encoder_id : cencs[0]};
		if (ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &enc)) { perror("GETENCODER"); return 1; }
		uint32_t crtc_id = enc.crtc_id;
		for (uint32_t i = 0; !crtc_id && i < res.count_crtcs; i++)
			if (enc.possible_crtcs & (1u << i))
				crtc_id = crtcs[i];

		struct drm_mode_create_dumb db = {.width = m->hdisplay, .height = m->vdisplay, .bpp = 32};
		if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &db)) { perror("CREATE_DUMB"); return 1; }
		struct drm_mode_fb_cmd fb = {.width = db.width, .height = db.height, .pitch = db.pitch,
					     .bpp = 32, .depth = 24, .handle = db.handle};
		if (ioctl(fd, DRM_IOCTL_MODE_ADDFB, &fb)) { perror("ADDFB"); return 1; }
		struct drm_mode_map_dumb md = {.handle = db.handle};
		if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &md)) { perror("MAP_DUMB"); return 1; }
		uint8_t *px = mmap(0, db.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, md.offset);
		if (px == MAP_FAILED) { perror("mmap"); return 1; }
		static const uint32_t bars[] = {0xffffff, 0xff0000, 0x00ff00, 0x0000ff};
		for (uint32_t y = 0; y < db.height; y++) {
			uint32_t *row = (uint32_t *)(px + y * db.pitch);
			for (uint32_t x = 0; x < db.width; x++)
				row[x] = bars[y * 4 / db.height];
		}

		struct drm_mode_crtc set = {.crtc_id = crtc_id, .fb_id = fb.fb_id, .set_connectors_ptr = U64(&conn.connector_id),
					    .count_connectors = 1, .mode = *m, .mode_valid = 1};
		if (ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &set)) {
			fprintf(stderr, "SETCRTC crtc %u: %s\n", crtc_id, strerror(errno));
			return 1;
		}
		printf("bars on crtc %u for %d s\n", crtc_id, secs);
		fflush(stdout);
		sleep(secs);
		return 0;
	}
	fprintf(stderr, "no connected connector\n");
	return 1;
}

/*
	KoboUSBMS: USBMS helper for KOReader
	Copyright (C) 2020 NiLuJe <ninuje@gmail.com>
	SPDX-License-Identifier: GPL-3.0-or-later

	This program is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include "usbms.h"

// Wrapper function to use syslog as a libevdev log handler
static void libevdev_to_syslog(const struct libevdev*     dev,
			       enum libevdev_log_priority priority,
			       void*                      data,
			       const char*                file,
			       int                        line,
			       const char*                func,
			       const char*                format,
			       va_list                    args) __attribute__((format(printf, 7, 0)));
static void
    libevdev_to_syslog(const struct libevdev*     dev,
		       enum libevdev_log_priority priority,
		       void*                      data,
		       const char*                file,
		       int                        line,
		       const char*                func,
		       const char*                format,
		       va_list                    args)
{
	syslog(LOG_INFO, "libdevdev: %s @ %s:%d (prio: %d)", func, file, line, priority);
	syslog(LOG_INFO, format, args);
}

static void
    setup_usb_ids(unsigned short int device_code)
{
	// Map device IDs to USB produc IDs, as we're going to need that in the scripts
	// c.f., https://github.com/kovidgoyal/calibre/blob/9d881ed2fcff219887579571f1bb48bdf41437d4/src/calibre/devices/kobo/driver.py#L1402-L1416
	// c.f., https://github.com/baskerville/plato/blob/d96e40737060b569ae875f37d6d741fd5ccc802c/contrib/plato.sh#L38-L56
	// NOTE: Keep 'em in FBInk order to make my life easier

	uint32_t pid = 0xDEAD;
	switch (device_code) {
		case 310U:    // Touch A/B (trilogy)
			pid = 0x4163;
			break;
		case 320U:    // Touch C (trilogy)
			pid = 0x4163;
			break;
		case 340U:    // Mini (pixie)
			pid = 0x4183;
			break;
		case 330U:    // Glo (kraken)
			pid = 0x4173;
			break;
		case 371U:    // Glo HD (alyssum)
			pid = 0x4223;
			break;
		case 372U:    // Touch 2.0 (pika)
			pid = 0x4224;
			break;
		case 360U:    // Aura (phoenix)
			pid = 0x4203;
			break;
		case 350U:    // Aura HD (dragon)
			pid = 0x4193;
			break;
		case 370U:    // Aura H2O (dahlia)
			pid = 0x4213;
			break;
		case 374U:    // Aura H2O² (snow)
			pid = 0x4227;
			break;
		case 378U:    // Aura H2O² r2 (snow)
			pid = 0x4227;
			break;
		case 373U:    // Aura ONE (daylight)
			pid = 0x4225;
			break;
		case 381U:    // Aura ONE LE (daylight)
			pid = 0x4225;
			break;
		case 375U:    // Aura SE (star)
			pid = 0x4226;
			break;
		case 379U:    // Aura SE r2 (star)
			pid = 0x4226;
			break;
		case 376U:    // Clara HD (nova)
			pid = 0x4228;
			break;
		case 377U:    // Forma (frost)
			pid = 0x4229;
			break;
		case 380U:    // Forma 32GB (frost)
			pid = 0x4229;
			break;
		case 384U:    // Libra H2O (storm)
			pid = 0x4232;
			break;
		case 382U:    // Nia (luna)
			pid = 0x4230;
			break;
		case 0U:
			pid = 0x4163;
			break;
		default:
			PFLOG(LOG_WARNING, "Can't match device code (%hu) to an USB Product ID!", device_code);
			break;
	}

	// Push it to the env...
	char pid_str[7] = { 0 };
	snprintf(pid_str, sizeof(pid_str) - 1U, "0x%X", pid);
	setenv("USB_PRODUCT_ID", pid_str, 1);
}

static bool
    is_usb_plugged(int ntxfd)
{
	// Check if we're plugged in...
	unsigned long ptr = 0U;
	int           rc  = ioctl(ntxfd, CM_USB_Plug_IN, &ptr);
	if (rc == -1) {
		PFLOG(LOG_WARNING, "Failed to query USB status (ioctl: %m)");
	}
	return !!ptr;
}

// Return a fancy battery icon given the charge percentage...
static const char*
    get_battery_icon(uint8_t charge)
{
	if (charge >= 100) {
		return "\uf578";
	} else if (charge >= 90) {
		return "\uf581";
	} else if (charge >= 80) {
		return "\uf580";
	} else if (charge >= 70) {
		return "\uf57f";
	} else if (charge >= 60) {
		return "\uf57e";
	} else if (charge >= 50) {
		return "\uf57d";
	} else if (charge >= 40) {
		return "\uf57c";
	} else if (charge >= 30) {
		return "\uf57b";
	} else if (charge >= 20) {
		return "\uf57a";
	} else if (charge >= 10) {
		return "\uf579";
	} else {
		return "\uf582";
	}
}

// NOTE: Inspired from git's strtoul_ui @ git-compat-util.h
static int
    strtoul_hhu(const char* str, uint8_t* restrict result)
{
	// NOTE: We want to *reject* negative values (which strtoul does not)!
	if (strchr(str, '-')) {
		LOG(LOG_WARNING, "Passed a negative value (%s) to strtoul_hhu", str);
		return -EINVAL;
	}

	// Now that we know it's positive, we can go on with strtoul...
	char* endptr;
	errno                 = 0;    // To distinguish success/failure after call
	unsigned long int val = strtoul(str, &endptr, 10);

	if ((errno == ERANGE && val == ULONG_MAX) || (errno != 0 && val == 0)) {
		PFLOG(LOG_WARNING, "strtoul: %m");
		return -EINVAL;
	}

	// NOTE: It fact, always clamp to CHAR_MAX, since we may need to cast to a signed representation later.
	if (val > CHAR_MAX) {
		LOG(LOG_WARNING, "Passed a value larger than CHAR_MAX to strtoul_hhu, clamping it down to CHAR_MAX");
		val = CHAR_MAX;
	}

	if (endptr == str) {
		LOG(LOG_WARNING, "No digits were found in value '%s' assigned to a variable expecting an uint8_t", str);
		return -EINVAL;
	}

	// If we got here, strtoul() successfully parsed at least part of a number.
	// But we do want to enforce the fact that the input really was *only* an integer value.
	if (*endptr != '\0') {
		LOG(LOG_WARNING,
		    "Found trailing characters (%s) behind value '%lu' assigned from string '%s' to a variable expecting an uint8_t",
		    endptr,
		    val,
		    str);
		return -EINVAL;
	}

	// Make sure there isn't a loss of precision on this arch when casting explicitly
	if ((uint8_t) val != val) {
		LOG(LOG_WARNING, "Loss of precision when casting value '%lu' to an uint8_t.", val);
		return -EINVAL;
	}

	*result = (uint8_t) val;
	return EXIT_SUCCESS;
}

static void
    print_status(int fbfd, const FBInkConfig* fbink_cfg, const FBInkOTConfig* ot_cfg, int ntxfd)
{
	// We'll want to display the plug/charge status, and whether Wi-Fi is on or not

	// Check if we're plugged in...
	bool usb_plugged = is_usb_plugged(ntxfd);

	// Get the battery charge %
	char  batt_charge[8] = { 0 };
	FILE* f              = fopen("/sys/devices/platform/pmic_battery.1/power_supply/mc13892_bat/capacity", "re");
	if (f) {
		size_t size = fread(batt_charge, sizeof(*batt_charge), sizeof(batt_charge), f);
		if (size > 0) {
			// NUL terminate
			batt_charge[size - 1U] = '\0';
			// Strip trailing LF
			if (batt_charge[size - 2U] == '\n') {
				batt_charge[size - 2U] = '\0';
			}
		}
		fclose(f);
	}
	uint8_t batt_perc = 0U;
	if (strtoul_hhu(batt_charge, &batt_perc) < 0) {
		LOG(LOG_WARNING, "Failed to convert battery charge value '%s' to an uint8_t!", batt_charge);
	}

	// Check for Wi-Fi (c.f., https://github.com/koreader/koreader/blob/b5d33058761625111d176123121bcc881864a64e/frontend/device/kobo/device.lua#L451-L471)
	bool wifi_up            = false;
	char if_sysfs[PATH_MAX] = { 0 };
	snprintf(if_sysfs, sizeof(if_sysfs) - 1U, "/sys/class/net/%s/carrier", getenv("INTERFACE"));
	f = fopen(if_sysfs, "re");
	if (f) {
		char   carrier[8];
		size_t size = fread(carrier, sizeof(*carrier), sizeof(carrier), f);
		if (size > 0) {
			// NUL terminate
			carrier[size - 1U] = '\0';
		}
		fclose(f);

		// If there's a carrier, Wi-Fi is up.
		if (carrier[0] == '1') {
			wifi_up = true;
		}
	}

	fbink_printf(fbfd,
		     ot_cfg,
		     fbink_cfg,
		     "%s • %s (%hhu%%) • %s",
		     usb_plugged ? "\ufba3" : "\ufba4",
		     get_battery_icon(batt_perc),
		     batt_perc,
		     wifi_up ? "\ufaa8" : "\ufaa9");
}

int
    main(void)
{
	// So far, so good ;).
	int                    rv       = EXIT_SUCCESS;
	int                    pwd      = -1;
	char*                  abs_pwd  = NULL;
	int                    fbfd     = -1;
	struct uevent_listener listener = { 0 };
	listener.pfd.fd                 = -1;
	struct libevdev* dev            = NULL;
	int              evfd           = -1;
	int              ntxfd          = -1;

	// We'll be chatting exclusively over syslog, because duh.
	openlog("usbms", LOG_CONS | LOG_PID | LOG_NDELAY, LOG_DAEMON);

	// Say hello
	LOG(LOG_INFO, "Initializing USBMS %s (%s)", USBMS_VERSION, USBMS_TIMESTAMP);

	// We'll want to jump to /, and only get back to our original PWD on exit...
	// c.f., man getcwd for the fchdir trick, as we can certainly spare the fd ;).
	// NOTE: O_PATH is Linux 2.6.39+ :(
	pwd = open(".", O_RDONLY | O_DIRECTORY | O_PATH | O_CLOEXEC);
	if (pwd == -1) {
		PFLOG(LOG_CRIT, "open: %m");
		rv = EXIT_FAILURE;
		goto cleanup;
	}
	// We do need the pathname to load resources, though...
	abs_pwd = get_current_dir_name();
	if (chdir("/") == -1) {
		PFLOG(LOG_CRIT, "chdir: %m");
		rv = EXIT_FAILURE;
		goto cleanup;
	}

	// Setup FBInk
	FBInkConfig fbink_cfg = { 0 };
	fbink_cfg.row         = -5;
	fbink_cfg.is_centered = true;
	fbink_cfg.is_padded   = true;
	fbink_cfg.to_syslog   = true;
	// We'll want early errors to already go to syslog
	fbink_update_verbosity(&fbink_cfg);

	if ((fbfd = fbink_open()) == ERRCODE(EXIT_FAILURE)) {
		LOG(LOG_CRIT, "Failed to open the framebuffer, aborting . . .");
		rv = EXIT_FAILURE;
		goto cleanup;
	}
	if (fbink_init(fbfd, &fbink_cfg) == ERRCODE(EXIT_FAILURE)) {
		LOG(LOG_CRIT, "Failed to initialize FBInk, aborting . . .");
		rv = EXIT_FAILURE;
		goto cleanup;
	}
	LOG(LOG_INFO, "Initialized FBInk %s", fbink_version());

	// Setup libue
	int rc = -1;
	rc     = ue_init_listener(&listener);
	if (rc < 0) {
		LOG(LOG_CRIT, "Failed to initialize libue listener (%d)", rc);
		rv = EXIT_FAILURE;
		goto cleanup;
	}
	LOG(LOG_INFO, "Initialized libue v%s", LIBUE_VERSION);

	// Setup libevdev
	evfd = open(NTX_KEYS_EVDEV, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
	if (evfd == -1) {
		PFLOG(LOG_CRIT, "open: %m");
		rv = EXIT_FAILURE;
		goto cleanup;
	}

	dev = libevdev_new();
	libevdev_set_device_log_function(dev, &libevdev_to_syslog, LIBEVDEV_LOG_INFO, NULL);
	rc = libevdev_set_fd(dev, evfd);
	if (rc < 0) {
		LOG(LOG_CRIT, "Failed to initialize libevdev (%s)", strerror(-rc));
		rv = EXIT_FAILURE;
		goto cleanup;
	}
	LOG(LOG_INFO, "Initialized libevdev v%s for device '%s'", LIBEVDEV_VERSION, libevdev_get_name(dev));

	// Now that FBInk has been initialized, setup the USB product ID for the current device
	FBInkState fbink_state = { 0 };
	fbink_get_state(&fbink_cfg, &fbink_state);
	setup_usb_ids(fbink_state.device_id);

	// Much like in KOReader's otamanager, check if we can use pipefail in a roundabout way,
	// because old busybox ash versions will *abort* on set failures...
	rc = system("set -o pipefail 2>/dev/null");
	if (rc == 0) {
		setenv("WITH_PIPEFAIL", "true", 1);
	} else {
		setenv("WITH_PIPEFAIL", "false", 1);
	}

	// Setup the fd for ntx_io ioctls
	ntxfd = open("/dev/ntx_io", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (ntxfd == -1) {
		PFLOG(LOG_CRIT, "open: %m");
		rv = EXIT_FAILURE;
		goto cleanup;
	}

	// Display our header
	fbink_cfg.no_refresh = true;
	fbink_cls(fbfd, &fbink_cfg, NULL);
	FBInkOTConfig ot_cfg         = { 0 };
	ot_cfg.margins.top           = fbink_state.font_h;
	ot_cfg.size_px               = fbink_state.font_h * 2U;
	char resource_path[PATH_MAX] = { 0 };
	snprintf(resource_path, sizeof(resource_path) - 1U, "%s/resources/fonts/CaskaydiaCove_NF.ttf", abs_pwd);
	if (fbink_add_ot_font(resource_path, FNT_REGULAR) != EXIT_SUCCESS) {
		PFLOG(LOG_CRIT, "Failed to load TTF font!");
		rv = EXIT_FAILURE;
		goto cleanup;
	}
	fbink_print_ot(fbfd, "USB Mass Storage", &ot_cfg, &fbink_cfg, NULL);
	fbink_cfg.ignore_alpha  = true;
	fbink_cfg.halign        = CENTER;
	fbink_cfg.scaled_height = fbink_state.screen_height / 10U;
	fbink_cfg.row           = 3;
	snprintf(resource_path, sizeof(resource_path) - 1U, "%s/resources/img/koreader.png", abs_pwd);
	fbink_print_image(fbfd, resource_path, 0, 0, &fbink_cfg);
	fbink_cfg.no_refresh  = false;
	fbink_cfg.is_flashing = true;
	fbink_refresh(fbfd, 0, 0, 0, 0, &fbink_cfg);
	fbink_cfg.is_flashing = false;

	// Display a minimal status bar on screen
	fbink_cfg.row      = -3;
	ot_cfg.margins.top = -(fbink_state.font_h * 3U);
	print_status(fbfd, &fbink_cfg, &ot_cfg, ntxfd);
	fbink_cfg.row = -5;

	// And now, on to the fun stuff!
	// If we're in USBNet mode, abort!
	// (tearing it down properly is out of our scope, since we can't really know how the user enabled it in the first place).
	rc = system("lsmod | grep -q g_ether");
	if (rc == 0) {
		LOG(LOG_ERR, "Device is in USBNet mode, aborting");

		// TODO: Switch to a fancy centered/scaled USBNet image & a /!\ OT icon in front of the message
		fbink_cfg.row = 5;
		fbink_print(fbfd, "Disable USBNet manually first!", &fbink_cfg);

		// TODO: Hold it for 30s/power button press, whichever comes first
		rv = EXIT_FAILURE;
		goto cleanup;
	}

cleanup:
	LOG(LOG_INFO, "Bye!");
	closelog();

	fbink_free_ot_fonts();
	fbink_close(fbfd);

	ue_destroy_listener(&listener);

	libevdev_free(dev);
	if (evfd != -1) {
		close(evfd);
	}

	if (ntxfd != -1) {
		close(ntxfd);
	}

	if (pwd != -1) {
		if (fchdir(pwd) == -1) {
			// NOTE: That would be bad, probably failed to remount onboard?
			PFLOG(LOG_CRIT, "fchdir: %m");
			rv = EXIT_FAILURE;
		}
		close(pwd);
	}
	free(abs_pwd);

	return rv;
}

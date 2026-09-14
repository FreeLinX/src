#ifndef FLX_LIBUDEV_H
#define FLX_LIBUDEV_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

struct udev;
struct udev_device;

struct udev *udev_new(void);
struct udev_device *udev_device_new_from_devnum(struct udev *udev, char type, dev_t devnum);
const char *udev_device_get_devpath(struct udev_device *udev_device);
const char *udev_device_get_syspath(struct udev_device *udev_device);
void udev_device_unref(struct udev_device *udev_device);
void udev_unref(struct udev *udev);

#ifdef __cplusplus
}
#endif

#endif
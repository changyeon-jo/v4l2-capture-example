# v4l2-capture-example
Use v4l2 capture devices in their DMABUF streaming I/O mode. DMABUFs
are allocated by DRM devices via minigbm and currently this
implementation supports V4L2 single-planar API only.

## Example
Below command will capture images from /dev/video0 and store them at
/data/vendor directory on the device.
```
> v4l2-capture-example /dev/dri/card1 /dev/video0
```

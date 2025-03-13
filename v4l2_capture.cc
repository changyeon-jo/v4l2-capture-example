#include <fcntl.h>
#include <gbm.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <poll.h>

#include <cstdio>
#include <cstring>  // For memset
#include <iostream>
#include <vector>

namespace {

struct buffer {
    gbm_bo* bo_handle;
    int dbuf_fd;
    size_t length;
};

struct stream {
    int v4l2_fd;
    int current_buffer;
    int buffer_count;
    std::vector<struct buffer> buffers;
};

}

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "Usage: " << argv[0] << " <drm_device> <v4l2_device>" << std::endl;
    return 1;
  }

  const char* drm_device = argv[1];
  const char* v4l2_device = argv[2];
  int v4l2_fd = -1;
  gbm_device* gbm = nullptr;

  // 1. Open V4L2 device
  v4l2_fd = open(v4l2_device, O_RDWR);
  if (v4l2_fd < 0) {
    std::cerr << "Failed to open v4l2 device." << std::endl;
    return EXIT_FAILURE;
  }

  // 2. Query V4L2 capabilities and format
  v4l2_capability cap;
  if (ioctl(v4l2_fd, VIDIOC_QUERYCAP, &cap) < 0) {
    std::cerr << "VIDIOC_QUERYCAP failed" << std::endl;
    return EXIT_FAILURE;
  }

  if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
    std::cerr << "V4L2 device does not support single-planar capture"
              << std::endl;
    return EXIT_FAILURE;
  }

  v4l2_format fmt;
  memset(&fmt, 0, sizeof(fmt));
  fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_ARGB32;
  fmt.fmt.pix.width = 1920;
  fmt.fmt.pix.height = 1020;

  if (ioctl(v4l2_fd, VIDIOC_S_FMT, &fmt) < 0) {
    std::cerr << "VIDIOC_S_FMT failed" << std::endl;
    return EXIT_FAILURE;
  }

  // Get the actual format that was set.
  if (ioctl(v4l2_fd, VIDIOC_G_FMT, &fmt) < 0) {
    std::cerr << "VIDIOC_G_FMT failed" << std::endl;
    return EXIT_FAILURE;
  }

  // 3. Initialize GBM
  int gbm_fd = open(drm_device, O_RDWR);  // Adjust card number if needed
  //int gbm_fd = open("/dev/dri/card0", O_RDWR);  // Adjust card number if needed
  //int gbm_fd = open("/dev/dri/renderD128", O_RDWR);  // Adjust card number if needed
  if (gbm_fd < 0) {
    std::cerr << "Failed to open DRM device: " << strerror(errno) << std::endl;
    return EXIT_FAILURE;
  }

  gbm = gbm_create_device(gbm_fd);
  if (gbm == nullptr) {
    std::cerr << "Failed to create GBM device" << std::endl;
    return EXIT_FAILURE;
  }

  // 4. Request V4L2 buffers (DMA-BUF)
  v4l2_requestbuffers req;
  memset(&req, 0, sizeof(req));
  req.count = 4;  // Number of buffers
  req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  req.memory = V4L2_MEMORY_DMABUF;

  if (ioctl(v4l2_fd, VIDIOC_REQBUFS, &req) < 0) {
    std::cerr << "VIDIOC_REQBUFS failed" << std::endl;
    return EXIT_FAILURE;
  }

  // 5. Allocate GBM buffers based on V4L2 buffer information
  v4l2_buffer buf;
  memset(&buf, 0, sizeof(buf));
  buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  buf.memory = V4L2_MEMORY_DMABUF;

  std::vector<struct buffer> buffers;
  for (unsigned int i = 0; i < req.count; ++i) {
    memset(&buf, 0, sizeof buf);
    buf.type = req.type;
    buf.memory = req.memory;
    buf.index = i;
    if (ioctl(v4l2_fd, VIDIOC_QUERYBUF, &buf) < 0) {
      std::cerr << "VIDIOC_QUERYBUF failed: " << strerror(errno) << std::endl;
      return EXIT_FAILURE;
    }

    uint32_t width = fmt.fmt.pix.width;
    uint32_t height = fmt.fmt.pix.height;
    uint32_t format = GBM_FORMAT_ARGB8888;  // gbm_bo_create() will fail with
                                            // GBM_FORMAT_BGRA8888;
    uint32_t flags = GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING;
    std::cout << "width=" << width << ", height=" << height << ", idx=" << i
              << std::endl;
    gbm_bo* bo = gbm_bo_create(gbm, width, height, format, flags);
    if (bo == nullptr) {
      std::cerr << "Failed to create GBM buffer: " << strerror(errno)
                << std::endl;
      return EXIT_FAILURE;
    }

    buf.m.fd = gbm_bo_get_fd(bo);
    if (ioctl(v4l2_fd, VIDIOC_QBUF, &buf) < 0) {
      std::cerr << "VIDIOC_QBUF failed: " << strerror(errno) << std::endl;
      return EXIT_FAILURE;
    }

    struct buffer b = {
        .bo_handle = bo,
        .dbuf_fd = buf.m.fd,
        .length = fmt.fmt.pix.bytesperline,
    };

    buffers.push_back(b);

  }

  // 6. Start streaming
  int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (ioctl(v4l2_fd, VIDIOC_STREAMON, &type) < 0) {
    std::cerr << "VIDIOC_STREAMON failed: " << strerror(errno) << std::endl;
    return EXIT_FAILURE;
  }

  std::cout << "Start capturing 2 frames..." << std::endl;

  // 7. Capture frames
  struct pollfd fds[] = {
      { .fd = v4l2_fd, .events = POLLIN },
      { .fd = gbm_fd, .events = POLLIN },
  };

  struct stream stream {
      .v4l2_fd = v4l2_fd,
      .current_buffer = -1,
      .buffers = buffers,
  };

  uint32_t frame_counter = 0;
  while (frame_counter++ < 5 && (poll(fds, /* nfds= */ 2, /* timeout= */ 5000) > 0)) {
    std::cerr << __LINE__ << std::endl;
    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_DMABUF;
    std::cout << "Waiting for DQBUF..." << std::endl;
    if (ioctl(v4l2_fd, VIDIOC_DQBUF, &buf) < 0) {
      std::cerr << "VIDIOC_DQBUF failed: " << strerror(errno) << std::endl;
      return EXIT_FAILURE;
    }

    // Process the captured frame (data is in the GBM buffer associated with
    // buf.index)
    std::cout << "Captured frame " << frame_counter << ", index: " << buf.index
              << std::endl;

    // Dump a frame
    uint32_t width = gbm_bo_get_width(buffers[buf.index].bo_handle);
    uint32_t height = gbm_bo_get_height(buffers[buf.index].bo_handle);
    uint32_t stride = gbm_bo_get_stride(buffers[buf.index].bo_handle);
    std::cout << "width=" << width << ", height=" << height << ", stride=" << stride << std::endl;
    void* map_metadata = nullptr;
    void* map_data = gbm_bo_map(buffers[buf.index].bo_handle, 0, 0, width, height,
                                GBM_BO_TRANSFER_READ, &stride, &map_metadata);
    std::cerr << __LINE__ << std::endl;
    if (map_data == nullptr) {
    std::cerr << __LINE__ << std::endl;
      std::cerr << "Failed to map a framebuffer!" << std::endl;
    } else {
    std::cerr << __LINE__ << std::endl;
      uint8_t* pixels = static_cast<uint8_t*>(map_data);
      std::string fname =
          std::string("/data/vendor/frame_") + std::to_string(frame_counter) + ".bin";
      uint32_t offset = 0; //buf.m.offset;
      uint32_t length = buf.bytesused;
      FILE* fp = fopen(fname.c_str(), "w");
    std::cerr << __LINE__ << std::endl;
      if (fp == NULL) {
        std::cerr << "Failed to open " << fname;
      } else {
        std::cout << "Writing " << length << " bytes from " << offset << " to "
                  << fname << std::endl;
        fwrite(pixels + offset, 1, length, fp);
      }
    std::cerr << __LINE__ << std::endl;
    }

    std::cerr << __LINE__ << std::endl;
    if (stream.current_buffer != -1) {
      v4l2_buffer buf;
      memset(&buf, 0, sizeof buf);
      buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      buf.memory = V4L2_MEMORY_DMABUF;
      buf.index = stream.current_buffer;
      buf.m.fd = stream.buffers[stream.current_buffer].dbuf_fd;
      if (ioctl(v4l2_fd, VIDIOC_QBUF, &buf) < 0) {
        std::cerr << "VIDIOC_QBUF failed" << std::endl;
        return EXIT_FAILURE;
      }
    }

    std::cerr << __LINE__ << std::endl;
    stream.current_buffer = buf.index;
  }

  if (frame_counter < 5) {
      std::cerr << "Capture loop is terminated unexpectedly; err=" << strerror(errno) << std::endl;
  }

  // 8. Stop streaming
  if (ioctl(v4l2_fd, VIDIOC_STREAMOFF, &type) < 0) {
    std::cerr << "VIDIOC_STREAMOFF failed" << std::endl;
    return EXIT_FAILURE;
  }

  // 9. Cleanup
  /*
  for (gbm_bo* bo : buffers) {
      gbm_bo_destroy(bo);
  }
  gbm_device_destroy(gbm);
  close(v4l2_fd);
  */

  if (gbm != nullptr) {
    gbm_device_destroy(gbm);
  }

  for (auto& b : buffers) {
    if (b.bo_handle != nullptr) {
      gbm_bo_destroy(b.bo_handle);
    }
  }
  gbm_device_destroy(gbm);
  close(gbm_fd);

  return EXIT_SUCCESS;
}

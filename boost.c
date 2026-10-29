#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>

#include <netinet/in.h>
#include <arpa/inet.h>

#include <linux/videodev2.h>

#define MAX_DEVICES 32
#define PORT 9000
#define WIDTH 1280
#define HEIGHT 720

struct buffer {
    void *start;
    size_t length;
};

static int xioctl(int fd, unsigned long request, void *arg)
{
    int ret;

    do {
        ret = ioctl(fd, request, arg);
    } while (ret == -1 && errno == EINTR);

    return ret;
}


static int find_camera(char *path, size_t path_size)
{
    for (int i = 0; i < MAX_DEVICES; i++) {

        snprintf(path, path_size, "/dev/video%d", i);

        int fd = open(path, O_RDWR | O_NONBLOCK);

        if (fd == -1)
            continue;

        struct v4l2_capability cap;

        memset(&cap, 0, sizeof(cap));

        if (xioctl(fd, VIDIOC_QUERYCAP, &cap) == -1) {
            close(fd);
            continue;
        }

        uint32_t capabilities = cap.capabilities;

        if (cap.capabilities & V4L2_CAP_DEVICE_CAPS)
            capabilities = cap.device_caps;

        if (capabilities & V4L2_CAP_VIDEO_CAPTURE) {

            printf("found ram buffer %s\n", path);
            return fd;
        }

        close(fd);
    }

    return -1;
}


static int send_all(int sock, const void *data, size_t length)
{
    const uint8_t *ptr = data;

    while (length > 0) {

        ssize_t sent = send(sock, ptr, length, 0);

        if (sent < 0) {

            if (errno == EINTR)
                continue;

            perror("send");
            return -1;
        }

        if (sent == 0)
            return -1;

        ptr += sent;
        length -= sent;
    }

    return 0;
}


int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <IPv6-address>\n", argv[0]);
        fprintf(stderr, "Example: %s 2400:1a00:1b48:93c0:b778:3c0e:cbb0:b072\n",
                argv[0]);
        return EXIT_FAILURE;
    }

    const char *ipv6 = argv[1];

    char device[64];

    int camera_fd = find_camera(device, sizeof(device));

    if (camera_fd == -1) {
        fprintf(stderr, "err\n");
        return EXIT_FAILURE;
    }

    struct v4l2_format fmt;

    memset(&fmt, 0, sizeof(fmt));

    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    fmt.fmt.pix.width = WIDTH;
    fmt.fmt.pix.height = HEIGHT;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    fmt.fmt.pix.field = V4L2_FIELD_ANY;

    if (xioctl(camera_fd, VIDIOC_S_FMT, &fmt) == -1) {
        perror("VIDIOC_S_FMT");

        close(camera_fd);
        return EXIT_FAILURE;
    }

    struct v4l2_requestbuffers req;

    memset(&req, 0, sizeof(req));

    req.count = 4;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (xioctl(camera_fd, VIDIOC_REQBUFS, &req) == -1) {
        perror("VIDIOC_REQBUFS");

        close(camera_fd);
        return EXIT_FAILURE;
    }

    if (req.count == 0) {
        fprintf(stderr, "err");

        close(camera_fd);
        return EXIT_FAILURE;
    }

    struct buffer *buffers =
        calloc(req.count, sizeof(struct buffer));

    if (!buffers) {
        perror("calloc");
        close(camera_fd);
        return EXIT_FAILURE;
    }


    for (unsigned int i = 0; i < req.count; i++) {

        struct v4l2_buffer buf;

        memset(&buf, 0, sizeof(buf));

        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (xioctl(camera_fd, VIDIOC_QUERYBUF, &buf) == -1) {
            perror("VIDIOC_QUERYBUF");
            return EXIT_FAILURE;
        }

        buffers[i].length = buf.length;

        buffers[i].start = mmap(
            NULL,
            buf.length,
            PROT_READ | PROT_WRITE,
            MAP_SHARED,
            camera_fd,
            buf.m.offset
        );

        if (buffers[i].start == MAP_FAILED) {
            perror("mmap");
            return EXIT_FAILURE;
        }
    }

    for (unsigned int i = 0; i < req.count; i++) {

        struct v4l2_buffer buf;

        memset(&buf, 0, sizeof(buf));

        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (xioctl(camera_fd, VIDIOC_QBUF, &buf) == -1) {
            perror("VIDIOC_QBUF");
            return EXIT_FAILURE;
        }
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (xioctl(camera_fd, VIDIOC_STREAMON, &type) == -1) {
        perror("VIDIOC_STREAMON");
        return EXIT_FAILURE;
    }

    printf("Camera streaming...\n");

    int sock = socket(AF_INET6, SOCK_STREAM, 0);

    if (sock == -1) {
        perror("socket");

        return EXIT_FAILURE;
    }

    struct sockaddr_in6 destination;

    memset(&destination, 0, sizeof(destination));

    destination.sin6_family = AF_INET6;
    destination.sin6_port = htons(PORT);

    if (inet_pton(AF_INET6, ipv6, &destination.sin6_addr) != 1) {

        fprintf(stderr, "Invalid IPv6 address: %s\n", ipv6);

        close(sock);
        return EXIT_FAILURE;
    }


    printf("Connecting to [%s]:%d...\n", ipv6, PORT);

    if (connect(
            sock,
            (struct sockaddr *)&destination,
            sizeof(destination)
        ) == -1) {

        perror("connect");

        close(sock);
        return EXIT_FAILURE;
    }

    printf("Connected!\n");
    struct v4l2_buffer buf;

    memset(&buf, 0, sizeof(buf));

    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;

    while (1) {

        if (xioctl(camera_fd, VIDIOC_DQBUF, &buf) == 0)
            break;

        if (errno == EAGAIN) {
            usleep(1000);
            continue;
        }

        perror("VIDIOC_DQBUF");

        close(sock);
        return EXIT_FAILURE;
    }


    printf(
        "Captured %u bytes\n",
        buf.bytesused
    );

    uint32_t image_size = htonl(buf.bytesused);

    if (send_all(sock, &image_size, sizeof(image_size)) == -1) {
        fprintf(stderr, "err");
        return EXIT_FAILURE;
    }

    if (send_all(
            sock,
            buffers[buf.index].start,
            buf.bytesused
        ) == -1) {

        fprintf(stderr, "err");
        return EXIT_FAILURE;
    }

    printf("success");


    xioctl(camera_fd, VIDIOC_STREAMOFF, &type);

    for (unsigned int i = 0; i < req.count; i++) {
        munmap(buffers[i].start, buffers[i].length);
    }

    free(buffers);

    close(sock);
    close(camera_fd);

    return EXIT_SUCCESS;
}

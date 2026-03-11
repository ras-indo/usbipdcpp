#include <asio.hpp>
#include <iostream>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <libusb-1.0/libusb.h>
#include <spdlog/spdlog.h>

#include "LibusbHandler/LibusbServer.h"

using namespace usbipdcpp;

// Environment variables used by termux-usb
#define ENV_SOCK_FD "TERMUX_ADB_SOCK_FD"
#define ENV_USB_FD  "TERMUX_USB_FD"

auto listen_port_env = "USBIPDCPP_LISTEN_PORT";

std::uint16_t parse_listen_port_from_env() {
    std::uint32_t listen_port;
    auto listen_port_str = std::getenv(listen_port_env);
    if (listen_port_str == nullptr) {
        spdlog::info("{} is not defined, use default port 3240", listen_port_env);
        listen_port = 3240;
    }
    else {
        if (sscanf(listen_port_str, "%u", &listen_port) != 1) {
            SPDLOG_WARN("Parse {} as int failed, use default port 3240", listen_port_env);
            listen_port = 3240;
        }
        else {
            spdlog::info("get listen port {} from {}", listen_port, listen_port_env);
        }
    }
    return static_cast<std::uint16_t>(listen_port);
}

// ==================== Termux auto-detection functions ====================

// Mendapatkan path perangkat USB pertama dari output 'termux-usb -l'
static std::string get_first_usb_device_path() {
    FILE* fp = popen("termux-usb -l", "r");
    if (!fp) {
        perror("popen");
        return "";
    }
    char buffer[256];
    std::string path;
    while (fgets(buffer, sizeof(buffer), fp)) {
        char* start = strchr(buffer, '"');
        if (start) {
            ++start;
            char* end = strchr(start, '"');
            if (end) {
                *end = '\0';
                path = start;
                break;
            }
        }
    }
    pclose(fp);
    return path;
}

// Child callback mode: dipanggil oleh termux-usb, mengirim USB fd kembali ke parent
static void child_callback_mode() {
    const char* sock_fd_str = getenv(ENV_SOCK_FD);
    const char* usb_fd_str  = getenv(ENV_USB_FD);
    if (!sock_fd_str || !usb_fd_str) {
        std::cerr << "[CHILD] Error: environment variables missing" << std::endl;
        exit(1);
    }

    int sock_fd = atoi(sock_fd_str);
    int usb_fd  = atoi(usb_fd_str);

    struct msghdr msg = {0};
    struct iovec iov;
    char cmsg_buf[CMSG_SPACE(sizeof(int))];
    char dummy = '@';

    iov.iov_base = &dummy;
    iov.iov_len  = 1;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsg_buf;
    msg.msg_controllen = sizeof(cmsg_buf);

    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    memcpy(CMSG_DATA(cmsg), &usb_fd, sizeof(int));

    if (sendmsg(sock_fd, &msg, 0) < 0) {
        perror("[CHILD] sendmsg");
        exit(1);
    }

    close(sock_fd);
    exit(0);
}

// Parent: menjalankan termux-usb dan menerima fd melalui socketpair
static int run_termux_usb(const std::string& device_path) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) < 0) {
        perror("socketpair");
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        close(sv[0]);
        close(sv[1]);
        return -1;
    }

    if (pid == 0) {
        // Child
        close(sv[0]);

        char sock_fd_str[16];
        snprintf(sock_fd_str, sizeof(sock_fd_str), "%d", sv[1]);
        setenv(ENV_SOCK_FD, sock_fd_str, 1);

        // Dapatkan path ke executable sendiri
        char self_path[256];
        ssize_t len = readlink("/proc/self/exe", self_path, sizeof(self_path)-1);
        if (len < 0) {
            perror("readlink");
            exit(1);
        }
        self_path[len] = '\0';

        execlp("termux-usb", "termux-usb", "-r", "-E", "-e", self_path, device_path.c_str(), NULL);
        perror("execlp termux-usb");
        exit(1);
    } else {
        // Parent
        close(sv[1]);

        struct msghdr msg = {0};
        struct iovec iov;
        char cmsg_buf[CMSG_SPACE(sizeof(int))];
        char recv_buf[1];

        iov.iov_base = recv_buf;
        iov.iov_len  = sizeof(recv_buf);
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = cmsg_buf;
        msg.msg_controllen = sizeof(cmsg_buf);

        ssize_t n = recvmsg(sv[0], &msg, 0);
        if (n < 0) {
            perror("[PARENT] recvmsg");
            close(sv[0]);
            wait(NULL);
            return -1;
        }

        int received_fd = -1;
        struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
        if (cmsg && cmsg->cmsg_len == CMSG_LEN(sizeof(int)) &&
            cmsg->cmsg_level == SOL_SOCKET &&
            cmsg->cmsg_type == SCM_RIGHTS) {
            memcpy(&received_fd, CMSG_DATA(cmsg), sizeof(int));
            std::cout << "[PARENT] Received USB fd: " << received_fd << std::endl;
        } else {
            std::cout << "[PARENT] No fd received" << std::endl;
        }

        close(sv[0]);
        wait(NULL);
        return received_fd;
    }
}

// ==================== main ====================

int main(int argc, char **argv) {
    // Unbuffered stdout/stderr
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    spdlog::set_level(spdlog::level::trace);
    spdlog::flush_on(spdlog::level::trace);

    // Cek jika dipanggil sebagai child oleh termux-usb (mode callback)
    if (std::getenv(ENV_SOCK_FD) != nullptr) {
        child_callback_mode();
        // tidak akan kembali
    }

    int fd = -1;
    std::uint16_t listen_port = parse_listen_port_from_env();

    // Mode operasi:
    // - Jika tidak ada argumen (argc == 1) -> auto mode: cari perangkat USB pertama
    // - Jika ada argumen (argc > 1)       -> mode kompatibilitas: argumen adalah fd
    if (argc <= 1) {
        // Auto mode
        spdlog::info("Auto mode: mencari perangkat USB pertama...");
        std::string dev_path = get_first_usb_device_path();
        if (dev_path.empty()) {
            SPDLOG_ERROR("Tidak ada perangkat USB terdeteksi.");
            return -1;
        }
        spdlog::info("Menggunakan perangkat: {}", dev_path);
        fd = run_termux_usb(dev_path);
        if (fd < 0) {
            SPDLOG_ERROR("Gagal mendapatkan file descriptor USB.");
            return -1;
        }
        spdlog::info("Berhasil mendapatkan fd = {}", fd);
    } else {
        // Mode kompatibilitas: argumen pertama adalah fd (angka)
        if (sscanf(argv[1], "%d", &fd) != 1) {
            SPDLOG_ERROR("Parse fd failed");
            return -1;
        }
        spdlog::info("Menggunakan fd = {} (dari argumen)", fd);
    }

    // Inisialisasi libusb
    libusb_set_option(nullptr, LIBUSB_OPTION_WEAK_AUTHORITY);
    int err = libusb_init(nullptr);
    if (err) {
        SPDLOG_ERROR("libusb_init failed: {}", libusb_strerror(err));
        libusb_exit(nullptr);
        return 1;
    }

    // Buat server
    LibusbServer server;
    asio::ip::tcp::endpoint endpoint(asio::ip::tcp::v4(), listen_port);

    // Bungkus fd menjadi libusb_device_handle
    libusb_device_handle *dev_handle;
    err = libusb_wrap_sys_device(nullptr, (intptr_t) fd, &dev_handle);
    if (err) {
        SPDLOG_ERROR("libusb_wrap_sys_device failed: {}", libusb_strerror(err));
        libusb_exit(nullptr);
        return -1;
    }

    // Bind device ke server
    server.bind_host_device(nullptr, true, dev_handle);
    server.start(endpoint);

    spdlog::info("Server berjalan pada port {}. Tekan Enter untuk berhenti...", listen_port);

    // Tunggu input dari user untuk berhenti
    std::string line;
    std::getline(std::cin, line);

    server.stop();
    libusb_exit(nullptr);
    spdlog::info("Server berhenti.");
    return 0;
}

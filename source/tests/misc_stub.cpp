/*
 * Minimal stub of misc.cpp functions for host-side unit tests.
 * Provides only ReadFile() which GetTopAppNameProcfs depends on.
 * The full misc.cpp requires scnlib which has host-compiler issues.
 */

#include "utils/misc.h"
#include <fcntl.h>
#include <unistd.h>

int ReadFile(const std::string_view &path, std::string *content, size_t maxLen) {
    constexpr size_t READ_DEFAULT_SIZE = 4096;
    if (content == nullptr) {
        return -1;
    }
    content->clear();

    int fd = open(path.data(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd <= 0) {
        return -1;
    }

    if (maxLen == 0) {
        maxLen = READ_DEFAULT_SIZE;
    }
    content->resize(maxLen);
    maxLen--;

    if (maxLen < READ_DEFAULT_SIZE) {
        auto len = read(fd, content->data(), maxLen);
        if (len > 0) {
            content->data()[len] = '\0';
            content->resize(len);
        } else {
            content->data()[0] = '\0';
            content->clear();
        }
        close(fd);
        return len;
    }

    int len = 0;
    int l;
    while ((l = read(fd, content->data() + len, maxLen - len)) > 0) {
        len += l;
    }
    if (len > 0) {
        content->data()[len] = '\0';
        content->resize(len);
    } else {
        content->data()[0] = '\0';
        content->clear();
    }
    close(fd);
    return len;
}

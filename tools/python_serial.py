import os
import struct
import fcntl

fd = os.open("/dev/ttyS4", os.O_RDONLY | os.O_NOCTTY)

buf = bytearray(44)
fcntl.ioctl(fd, 0x802C542A, buf)

struct.pack_into(
    "I",
    buf,
    0,
    struct.unpack_from("I", buf, 0)[0] | 0x1000,
)
struct.pack_into("I", buf, 36, 420000)
struct.pack_into("I", buf, 40, 420000)

fcntl.ioctl(fd, 0x402C542B, buf)

try:
    while True:
        d = os.read(fd, 26)
        if d:
            print(" ".join(f"{b:02x}" for b in d))
except KeyboardInterrupt:
    pass
finally:
    os.close(fd)

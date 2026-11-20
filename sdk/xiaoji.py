import ctypes
from pathlib import Path


class Xiaoji:
    KEYBOARD = 1
    MOUSE = 2

    def __init__(self, library=None):
        path = Path(library) if library is not None else Path(__file__).with_name("xiaoji_client.dll")
        self._dll = ctypes.CDLL(str(path.resolve(strict=True)))
        self._bind("XiaojiGetAbiVersion", ctypes.c_uint32)
        if self._dll.XiaojiGetAbiVersion() != 2:
            raise RuntimeError("Unsupported xiaoji client ABI (expected 2)")
        self._bind("XiaojiLastError", ctypes.c_char_p)
        self._bind("XiaojiGetDevices", ctypes.c_int, ctypes.POINTER(ctypes.c_uint32))
        self._bind("XiaojiCheckConnection", ctypes.c_int, ctypes.c_uint32)
        self._bind("XiaojiTapKey", ctypes.c_int, ctypes.c_uint8, ctypes.c_uint8)
        self._bind("XiaojiTypeAscii", ctypes.c_int, ctypes.c_char_p, ctypes.c_uint32)
        self._bind("XiaojiMoveMouse", ctypes.c_int, ctypes.c_int32, ctypes.c_int32, ctypes.c_int32)

    def _bind(self, name, result_type, *argument_types):
        function = getattr(self._dll, name)
        function.restype = result_type
        function.argtypes = list(argument_types)

    def _check(self, result):
        if result:
            detail = self._dll.XiaojiLastError().decode("utf-8", errors="replace")
            raise (ValueError if result == 1 else RuntimeError)(detail)

    def devices(self):
        mask = ctypes.c_uint32()
        self._check(self._dll.XiaojiGetDevices(ctypes.byref(mask)))
        return mask.value

    def check_connection(self, device):
        if device not in (self.KEYBOARD, self.MOUSE):
            raise ValueError("Select keyboard or mouse")
        self._check(self._dll.XiaojiCheckConnection(device))

    def tap(self, usage, modifiers=0):
        if not (0x04 <= usage <= 0x65 and 0 <= modifiers <= 0xff):
            raise ValueError("Invalid keyboard usage or modifier mask")
        self._check(self._dll.XiaojiTapKey(modifiers, usage))

    def type_ascii(self, text):
        data = text.encode("ascii")
        if not 1 <= len(data) <= 256:
            raise ValueError("Expected 1..256 ASCII bytes")
        self._check(self._dll.XiaojiTypeAscii(data, len(data)))

    def move(self, dx, dy, wheel=0):
        if not all(-127 <= value <= 127 for value in (dx, dy, wheel)):
            raise ValueError("Mouse values must each be in -127..127")
        self._check(self._dll.XiaojiMoveMouse(dx, dy, wheel))


if __name__ == "__main__":
    client = Xiaoji()
    devices = client.devices()
    print(f"keyboard={bool(devices & client.KEYBOARD)}, mouse={bool(devices & client.MOUSE)}")

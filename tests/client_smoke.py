import ctypes
import sys
import threading
import unittest
from pathlib import Path

SDK = Path(sys.argv.pop(1)).resolve()
sys.path.insert(0, str(SDK))
from xiaoji import Xiaoji


class ClientSmoke(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.client = Xiaoji(SDK / "xiaoji_client.dll")

    def test_device_query(self):
        self.assertIn(self.client.devices(), (0, 1, 2, 3))

    def test_native_rejects_invalid_input_without_sending(self):
        native = self.client._dll
        for function, args in (
            (native.XiaojiGetDevices, (None,)),
            (native.XiaojiCheckConnection, (3,)),
            (native.XiaojiTapKey, (0, 0x66)),
            (native.XiaojiTypeAscii, (None, 1)),
            (native.XiaojiTypeAscii, (b"a\0", 2)),
            (native.XiaojiTypeAscii, (b"a" * 257, 257)),
            (native.XiaojiMoveMouse, (0, 0, 128)),
        ):
            with self.subTest(function=function.__name__):
                self.assertEqual(function(*args), 1)
                self.assertTrue(native.XiaojiLastError())

    def test_errors_are_per_thread(self):
        native = self.client._dll
        self.assertEqual(native.XiaojiGetDevices(None), 1)
        before = native.XiaojiLastError()
        other = []

        def read_worker_error():
            native.XiaojiTapKey(0, 0)
            other.append(native.XiaojiLastError())

        worker = threading.Thread(target=read_worker_error)
        worker.start()
        worker.join()
        self.assertTrue(other[0])
        self.assertNotEqual(other[0], before)
        self.assertEqual(native.XiaojiLastError(), before)
        mask = ctypes.c_uint32()
        self.assertEqual(native.XiaojiGetDevices(ctypes.byref(mask)), 0)
        self.assertEqual(native.XiaojiLastError(), b"")

    def test_session_invalid_arguments_only(self):
        native = self.client._dll
        handle = ctypes.c_void_p
        word = ctypes.c_uint32
        for name, args in (
            ("Open", (word, word, word, ctypes.POINTER(handle))),
            ("Key", (handle, ctypes.c_uint8, ctypes.c_int)),
            ("Button", (handle, word, ctypes.c_int)),
            ("Wheel", (handle, ctypes.c_int32)),
            ("Close", (handle,)),
        ):
            self.client._bind("XiaojiSession" + name, ctypes.c_int, *args)

        for devices, ready, ack in ((0, 1, 1), (4, 1, 1), (1, 0, 1), (1, 10001, 1), (2, 1, 0), (2, 1, 2001)):
            with self.subTest(devices=devices, ready=ready, ack=ack):
                output = handle(1)
                self.assertEqual(native.XiaojiSessionOpen(devices, ready, ack, ctypes.byref(output)), 1)
                self.assertTrue(native.XiaojiLastError())
                self.assertIsNone(output.value)
        for function, args in (
            (native.XiaojiSessionOpen, (1, 1, 1, None)),
            (native.XiaojiSessionKey, (None, 4, 1)),
            (native.XiaojiSessionButton, (None, 1, 1)),
            (native.XiaojiSessionWheel, (None, 1)),
            (native.XiaojiSessionClose, (None,)),
        ):
            with self.subTest(function=function.__name__):
                self.assertEqual(function(*args), 1)
                self.assertTrue(native.XiaojiLastError())

    def test_wrapper_rejects_overflow_before_ffi_conversion(self):
        with self.assertRaises(ValueError):
            self.client.tap(0x104)
        with self.assertRaises(ValueError):
            self.client.move(2**32, 0)
        with self.assertRaises(ValueError):
            self.client.type_ascii("a\r")


if __name__ == "__main__":
    unittest.main()

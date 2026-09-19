import unittest
from package import validate_config, validate_payload


class PackageGuards(unittest.TestCase):
    config = "\n".join(f"CONFIG_{name}=y" for name in ("SENSOR", "I2C", "NVS", "SOC_AMEBA_NP_IMAGE"))

    def test_expected_config(self):
        validate_config(self.config)

    def test_reject_network_and_no_blob(self):
        for name in ("WIFI", "NETWORKING", "BUILD_ONLY_NO_BLOBS"):
            with self.assertRaises(ValueError):
                validate_config(self.config + f"\nCONFIG_{name}=y")

    def test_require_full_image(self):
        with self.assertRaises(ValueError):
            validate_config("CONFIG_SENSOR=y")

    def test_privacy(self):
        for data in (b"/Users/example/private", b"example@gmail.com"):
            with self.assertRaises(ValueError):
                validate_payload({"firmware.bin": data})
        validate_payload({"firmware.bin": b"no identifiers"})

    def test_archive_paths(self):
        for name in ("/absolute", "../escape"):
            with self.assertRaises(ValueError):
                validate_payload({name: b"test"})


if __name__ == "__main__":
    unittest.main()

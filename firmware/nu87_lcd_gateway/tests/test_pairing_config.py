import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("gateway_pairing", Path(__file__).resolve().parents[1] / "pairing_config.py")
pairing = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pairing)


class PairingConfigTests(unittest.TestCase):
    def test_private_config_and_invalid_source(self):
        with tempfile.TemporaryDirectory() as directory:
            source, target = Path(directory) / "code", Path(directory) / "generated.conf"
            source.write_text("095182\n")
            source.chmod(0o600)
            pairing.write_pairing_config(source, target)
            self.assertEqual(target.read_text(), 'CONFIG_GATEWAY_NU40_PASSKEY="095182"\n')
            self.assertEqual(target.stat().st_mode & 0o777, 0o600)
            source.chmod(0o644)
            with self.assertRaises(ValueError):
                pairing.write_pairing_config(source, target)
            source.chmod(0o600)
            source.write_text("invalid-private-secret")
            with self.assertRaises(ValueError) as error:
                pairing.write_pairing_config(source, target)
            self.assertNotIn("invalid-private-secret", str(error.exception))
            source.write_text("095182")
            link = Path(directory) / "link"
            link.symlink_to(source)
            with self.assertRaises(ValueError):
                pairing.write_pairing_config(link, target)
            with self.assertRaises(ValueError):
                pairing.write_pairing_config(source, link)


if __name__ == "__main__":
    unittest.main()

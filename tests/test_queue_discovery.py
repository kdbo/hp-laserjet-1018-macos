"""Regression checks for locale-independent CUPS queue discovery.

Run with: python3 -m unittest discover -s tests
"""
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WATCH = ROOT / "pkg/root/usr/local/libexec/hp1018/watch.sh"
AIRPRINT = ROOT / "pkg/root/Library/Application Support/HP1018/airprint.sh"
URI = "usb://Hewlett-Packard/HP%20LaserJet%201018"
CONF = f"""<Printer Office>
Info Office printer
DeviceURI {URI}?serial=ONE
Shared Yes
</Printer>
<DefaultPrinter Default>
Info Default printer
Location Upstairs
DeviceURI {URI}?serial=TWO
Shared Yes
</DefaultPrinter>
<Printer Other>
DeviceURI usb://Hewlett-Packard/HP%20LaserJet%201020?serial=OTHER
</Printer>
<Printer MissingURI>
Shared Yes
</Printer>
<Printer NoSerial>
DeviceURI {URI}
Shared No
</Printer>
"""


class QueueDiscoveryTests(unittest.TestCase):
    def run_function(self, script, command):
        # Load function definitions only; never start the daemon or touch services.
        definitions = script.read_text().split("\nensure_state_dir\n", 1)[0]
        with tempfile.NamedTemporaryFile(mode="w") as config:
            config.write(CONF)
            config.flush()
            return subprocess.run(
                ["/bin/bash", "-c", definitions +
                 '\nPRINTERS_CONF="$1"\n' + command, "test", config.name],
                text=True, capture_output=True,
            )

    def test_both_daemons_find_exact_uris(self):
        expected = (f"Office\t{URI}?serial=ONE\n"
                    f"Default\t{URI}?serial=TWO\n"
                    f"NoSerial\t{URI}\n")
        for script in (WATCH, AIRPRINT):
            with self.subTest(script=script):
                result = self.run_function(script, "list_hp1018_queues")
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout, expected)

    def test_airprint_default_queue_metadata(self):
        result = self.run_function(AIRPRINT,
            'get_queue_device_uri Default; '
            'get_printer_value Default Info; '
            'get_printer_value Default Location; is_shared Default')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout,
                         f"{URI}?serial=TWO\nDefault printer\nUpstairs\n")

    def test_airprint_identity_and_sharing(self):
        for command, expected in (
            (f'queue_is_configured_for_uri Office "{URI}?serial=ONE"', 0),
            (f'queue_is_configured_for_uri Office "{URI}?serial=TWO"', 1),
            ('is_shared NoSerial', 1),
            ('is_shared Unknown', 1),
        ):
            with self.subTest(command=command):
                result = self.run_function(AIRPRINT, command)
                self.assertEqual(result.returncode, expected, result.stderr)


if __name__ == "__main__":
    unittest.main()

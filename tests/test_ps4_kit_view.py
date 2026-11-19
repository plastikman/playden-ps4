"""The PS4 kit must not claim a capability it cannot keep.

Playden gates almost every per-game setting on the kit manifest's capabilities array, and the PS4
kit is synthesised by scripts/ps4-kit-view.py rather than packaged by prospero-win's own packager.
That packager refuses to write a kit whose capabilities disagree with its artifacts (an any-title
variant and title-any-id go together; Wine Mono provenance and wine-mono go together). Nothing held
the PS4 kit to the same standard, and a hand-written list drifted both ways:

  - opengl-zink went unclaimed while PS4 staged the provider the whole time, so The Binding of Isaac
    got a window and no rendering (2026-11-12).
  - profile-version-2 went unclaimed, so the 32-bit code row stayed locked and MDK could not reach
    the translator (2026-11-18).
  - game-root-unowned was claimed wrongly and crashed the launcher on its title screen (03.38).

These tests are that standard: a name must be one Playden knows, a claim with staged providers must
disappear when a provider does, and the capabilities we decided to refuse must stay refused with
their reason recorded.
"""

import importlib.util
from pathlib import Path
import re
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / 'scripts/ps4-kit-view.py'
KIT_TRUST = ROOT / 'src/runtime/kit_trust.cpp'
CAPABILITY_HEADER = ROOT / 'src/runtime/game_settings.h'


def load_script():
    spec = importlib.util.spec_from_file_location('ps4_kit_view', SCRIPT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def playden_capability_names():
    """Every capability name KitCapabilitiesFromManifest actually reads.

    This -- not kit_trust.cpp -- is the authority for PS4. kit_trust.cpp's known_capabilities
    filters a PS5 kit in kit_manifest.cpp, which drops an unrecognised name into
    ignored_capabilities. The PS4 path does not go through that validator: kit_ps4.cpp pushes every
    string from playden-kit-view.json straight into kit.capabilities, and runtime_host.cpp hands
    that list to KitCapabilitiesFromManifest, which compares strings directly. So a name works on
    PS4 iff game_settings.h defines it as a kKit* constant that function consults.
    """
    return set(re.findall(r'kKit[A-Za-z0-9]+\s*=\s*"([a-z0-9-]+)"',
                          CAPABILITY_HEADER.read_text(encoding='utf-8')))


def ps5_admission_names():
    """What a PS5 kit's manifest may declare (kit_trust.cpp). Narrower than the above."""
    constants = dict(re.findall(r'(kKit[A-Za-z0-9]+)\s*=\s*"([a-z0-9-]+)"',
                                CAPABILITY_HEADER.read_text(encoding='utf-8')))
    body = re.search(r'known_capabilities\s*=\s*\{(.*?)\};',
                     KIT_TRUST.read_text(encoding='utf-8'), re.S)
    assert body, 'kit_trust.cpp no longer spells known_capabilities the way this test reads it'
    names = set(re.findall(r'"([a-z0-9-]+)"', body.group(1)))
    names.update(constants[c] for c in re.findall(r'\b(kKit[A-Za-z0-9]+)\b', body.group(1))
                 if c in constants)
    return names


class Ps4KitViewCapabilityTests(unittest.TestCase):
    def setUp(self):
        self.view = load_script()
        self.known = playden_capability_names()

    def staged(self, providers):
        """A stage directory holding exactly these provider files."""
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        stage = Path(directory.name)
        for provider in providers:
            path = stage / provider
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b'provider')
        return stage

    def test_playden_vocabulary_was_actually_read(self):
        # Guards the regexes above: if either source is reshaped, every other test here would pass
        # vacuously against an empty set.
        self.assertGreater(len(self.known), 25)
        self.assertIn('title-any-id', self.known)

    def test_claims_outside_the_ps5_admission_list_are_deliberate(self):
        """PS4 reads capabilities kit_trust.cpp does not list, and that is fine here.

        kit_manifest.cpp would drop these into ignored_capabilities for a PS5 kit, but the PS4 path
        (kit_ps4.cpp -> runtime_host.cpp -> KitCapabilitiesFromManifest) never consults that list.
        Pinned so the difference is a decision on record rather than a surprise: if this set grows,
        check the new name really is read on the PS4 path before claiming it.
        """
        beyond = set(self.view.CAPABILITY_EVIDENCE) - ps5_admission_names()
        self.assertEqual({'input-touchpad', 'input-keyboard-stick'}, beyond)

    def test_every_claim_is_a_name_playden_knows(self):
        for name in self.view.CAPABILITY_EVIDENCE:
            with self.subTest(capability=name):
                self.assertIn(name, self.known,
                              f'{name} is not in kit_trust.cpp, so Playden ignores it')

    def test_every_refusal_is_a_name_playden_knows(self):
        # A typo here would silently stop protecting us against the real capability.
        for name in self.view.NEVER_CLAIM:
            with self.subTest(capability=name):
                self.assertIn(name, self.known)

    def test_refusals_carry_a_reason(self):
        for name, reason in self.view.NEVER_CLAIM.items():
            with self.subTest(capability=name):
                self.assertGreater(len(reason), 40,
                                   f'{name} needs a reason a later audit can act on')

    def test_a_capability_is_never_both_claimed_and_refused(self):
        self.assertEqual(set(), set(self.view.CAPABILITY_EVIDENCE) & set(self.view.NEVER_CLAIM))

    def test_refused_capabilities_stay_out_of_a_full_stage(self):
        every_provider = [p for providers, _ in self.view.CAPABILITY_EVIDENCE.values()
                          for p in providers]
        claimed = self.view.capabilities(self.staged(every_provider))
        for name in self.view.NEVER_CLAIM:
            with self.subTest(capability=name):
                self.assertNotIn(name, claimed)

    def test_a_claim_with_providers_is_dropped_when_one_is_missing(self):
        backed = {name: providers for name, (providers, _) in self.view.CAPABILITY_EVIDENCE.items()
                  if providers}
        self.assertTrue(backed, 'no capability is evidence-backed; the drift guard does nothing')
        every_provider = [p for providers in backed.values() for p in providers]
        for name, providers in backed.items():
            for missing in providers:
                with self.subTest(capability=name, without=missing):
                    kept = [p for p in every_provider if p != missing]
                    self.assertNotIn(name, self.view.capabilities(self.staged(kept)))

    def test_compiled_marker_backed_claims_require_their_binary_markers(self):
        for name, markers in self.view.CAPABILITY_MARKERS.items():
            with self.subTest(capability=name):
                checks = markers if isinstance(markers[0], tuple) else (markers,)
                paths = [path for path, _ in checks]
                stage = self.staged(paths)
                self.assertNotIn(name, self.view.capabilities(stage))
                for path, marker in checks:
                    (stage / path).write_bytes(b'prefix ' + marker + b' suffix')
                self.assertIn(name, self.view.capabilities(stage))
                if name == 'wine-driver-threads':
                    for path, _ in checks:
                        (stage / path).write_bytes(b'marker removed')
                        self.assertNotIn(name, self.view.capabilities(stage))
                        # Restore this provider before checking the next one.
                        marker = next(value for candidate, value in checks if candidate == path)
                        (stage / path).write_bytes(b'prefix ' + marker + b' suffix')
        self.assertNotIn('wine-driver-threads', self.view.NEVER_CLAIM)

    def test_parser_backed_claims_are_listed_without_any_staged_file(self):
        unbacked = sorted(name for name, (providers, _) in self.view.CAPABILITY_EVIDENCE.items()
                          if not providers)
        self.assertEqual(unbacked, self.view.capabilities(self.staged([])))

    def test_wine_mono_requires_payload_and_template_runtime_path(self):
        provider = self.view.CAPABILITY_EVIDENCE['wine-mono'][0][0]
        stage = self.staged([provider])
        self.assertNotIn('wine-mono', self.view.capabilities(stage))
        registry = stage / self.view.TEMPLATE / 'user.reg'
        registry.parent.mkdir(parents=True, exist_ok=True)
        expected = (f'"RuntimePath"="Z:\\\\data\\\\playden\\\\kits\\\\orbis-win-ps4'
                    f'\\\\mono\\\\wine-mono-{self.view.MONO_VERSION}"')
        registry.write_text(f'[Software\\\\Wine\\\\Mono]\n{expected}\n', encoding='utf-8')
        self.assertIn('wine-mono', self.view.capabilities(stage))
        registry.write_text('[Software\\\\Wine\\\\Mono]\n"RuntimePath"="Z:\\\\wrong"\n',
                            encoding='utf-8')
        self.assertNotIn('wine-mono', self.view.capabilities(stage))

    def test_vkd3d_proton_requires_all_architectures_and_hardware_qualification(self):
        providers = self.view.VKD3D_PROTON_PROVIDERS
        stage = self.staged(providers)
        for provider in providers:
            (stage / provider).write_bytes(b'PE test payload')
        with mock.patch.object(self.view, 'VKD3D_PROTON_QUALIFIED', False):
            self.assertNotIn('vkd3d-proton', self.view.capabilities(stage))
        with mock.patch.object(self.view, 'VKD3D_PROTON_QUALIFIED', True):
            self.assertIn('vkd3d-proton', self.view.capabilities(stage))
            (stage / providers[-1]).unlink()
            self.assertNotIn('vkd3d-proton', self.view.capabilities(stage))

    def test_display_120hz_is_never_claimed(self):
        # The one that would do real damage: Playden's SoftwareFrameLimit() turns OFF its software
        # frame limiter for a kit claiming this, and our runtime has no [display] frame_rate pacing,
        # so every game would run uncapped. [display] refresh accepting 120 is not evidence for it.
        self.assertIn('display-120hz', self.view.NEVER_CLAIM)
        self.assertNotIn('display-120hz', self.view.CAPABILITY_EVIDENCE)


if __name__ == '__main__':
    unittest.main()

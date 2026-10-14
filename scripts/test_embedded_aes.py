"""Run the pinned portable AES sources against independent NIST vector data."""
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
CRYPTO = ROOT / "src/Kernel/Extensions/25.5.0/embedded_crypto"


@unittest.skipUnless(shutil.which("cc"), "requires a native C compiler")
class AESTests(unittest.TestCase):
    def test_nist_cbc_vectors(self):
        vectors = json.loads((CRYPTO / "AES_SELFTEST.json").read_text())
        def initializer(value):
            return ",".join(f"0x{byte:02x}" for byte in bytes.fromhex(value))
        self.assertEqual(len(bytes.fromhex(vectors["plaintext"])), 64)
        for key, ciphertext in zip(vectors["keys"], vectors["ciphertext"]):
            self.assertIn(len(bytes.fromhex(key)), (16, 24, 32))
            self.assertEqual(len(bytes.fromhex(ciphertext)), 64)
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            source = work / "aes_test.c"
            source.write_text('''#include <string.h>
#include "bearssl_block.h"
int main(void) {
''' +
                f'const unsigned char plain[64] = {{{initializer(vectors["plaintext"])}}};\n' +
                f'const unsigned char initial_iv[16] = {{{initializer(vectors["iv"])}}};\n' +
                'unsigned char data[64], iv[16];\n'
                'br_aes_ct_cbcenc_keys enc; br_aes_ct_cbcdec_keys dec;\n' +
                ''.join('{' +
                    f'const unsigned char key[] = {{{initializer(key)}}};\n' +
                    f'const unsigned char expected[64] = {{{initializer(ciphertext)}}};\n' +
                    '''br_aes_ct_cbcenc_init(&enc, key, sizeof(key));
memcpy(data, plain, 64); memcpy(iv, initial_iv, 16);
br_aes_ct_cbcenc_run(&enc, iv, data, 64);
if (memcmp(data, expected, 64) || memcmp(iv, expected + 48, 16)) return 1;
br_aes_ct_cbcdec_init(&dec, key, sizeof(key)); memcpy(iv, initial_iv, 16);
br_aes_ct_cbcdec_run(&dec, iv, data, 16);
br_aes_ct_cbcdec_run(&dec, iv, data + 16, 48);
if (memcmp(data, plain, 64) || memcmp(iv, expected + 48, 16)) return 2;
}
''' for key, ciphertext in zip(vectors["keys"], vectors["ciphertext"])) + 'return 0; }\n')
            bearssl = CRYPTO / "bearssl"
            executable = work / "aes_test"
            names = ("aes_ct", "aes_ct_enc", "aes_ct_dec", "aes_ct_cbcenc", "aes_ct_cbcdec")
            subprocess.run(["cc", "-O2", "-I", str(bearssl / "inc"), "-I", str(bearssl / "src"),
                            str(source), *[str(bearssl / "src/symcipher" / (name + ".c")) for name in names],
                            "-o", str(executable)], check=True)
            subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    unittest.main()

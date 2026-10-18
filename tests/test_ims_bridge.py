"""ims_bridge's one parse of a packet from the modem (upstream/modules/ims_bridge/imsbr_parse.h, FINDINGS 192),
compiled on the host: the check, the classification (addresses, protocol, ports), the routing and the delivery all
come from it, so a packet it accepts is one whose every field it read lies within the bytes there are - IPv4 options
and total length, IPv6 payload length and extension headers, fragments without ports. Each packet is handed over in a
buffer of exactly its size, under AddressSanitizer where the compiler has it, and random packets must either be
refused or come back with fields that stay within them."""
import random
import shutil
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path

from helpers import TOP

HEADER = TOP / 'upstream/modules/ims_bridge/imsbr_parse.h'

# one packet per line, in hex; prints "bad", or the parse's fields
HARNESS = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "imsbr_parse.h"
int main(void)
{
	static char line[200000];

	while (fgets(line, sizeof(line), stdin)) {
		unsigned int n = strlen(line) / 2, i, b;
		unsigned char *p = malloc(n ? n : 1);
		struct imsbr_l3 l3;

		for (i = 0; i < n; i++) {
			sscanf(line + 2 * i, "%2x", &b);
			p[i] = b;
		}
		if (imsbr_l3_parse(p, n, &l3))
			puts("bad");
		else
			printf("%u %u %u %u %u %u %u %u %u %u\n", l3.family, l3.len, l3.l4off, l3.l4proto, l3.has_ports,
			       l3.sport, l3.dport, l3.saddr_off, l3.daddr_off, l3.alen);
		fflush(stdout);
		free(p);
	}
	return 0;
}
'''


def v4(payload=b'', proto=17, ihl=5, tot=None, frag=0, version=4, pad=b''):
    opts = b'\x01' * (ihl * 4 - 20) if ihl >= 5 else b''
    tot = 20 + len(opts) + len(payload) if tot is None else tot
    h = struct.pack('!BBHHHBBH4s4s', (version << 4) | ihl, 0, tot, 0, frag, 64, proto, 0, bytes([10, 0, 0, 1]),
                    bytes([10, 0, 0, 2]))
    return h + opts + payload + pad


def v6(payload=b'', nh=17, plen=None, version=6):
    plen = len(payload) if plen is None else plen
    return struct.pack('!IHBB16s16s', version << 28, plen, nh, 64, b'\x20' + b'\0' * 15, b'\x20' + b'\0' * 14 + b'\1') \
        + payload


UDP = struct.pack('!HHHH', 5004, 40000, 12, 0) + b'rtp!'


@unittest.skipUnless(shutil.which('cc'), 'needs a C compiler')
class Parse(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.dir = Path(tempfile.mkdtemp())
        (cls.dir / 'h.c').write_text(HARNESS)
        cls.exe = cls.dir / 'h'
        base = ['cc', '-std=c99', '-Wall', '-Werror', f'-I{HEADER.parent}', '-o', str(cls.exe), str(cls.dir / 'h.c')]
        # AddressSanitizer turns a read past a packet into a failure, where the compiler and the host have it
        if subprocess.run(base[:1] + ['-fsanitize=address,undefined'] + base[1:], capture_output=True).returncode:
            subprocess.run(base, check=True)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.dir)

    def parse(self, *pkts):
        r = subprocess.run([str(self.exe)], input=''.join(p.hex() + '\n' for p in pkts), capture_output=True,
                           text=True, check=True)
        self.assertEqual(r.stderr, '')
        out = []
        for l in r.stdout.splitlines():
            if l == 'bad':
                out.append(None)
            else:
                f = list(map(int, l.split()))
                out.append(dict(zip(('family', 'len', 'l4off', 'proto', 'ports', 'sport', 'dport', 'saddr', 'daddr',
                                     'alen'), f)))
        return out if len(pkts) > 1 else out[0]

    def test_ipv4_udp(self):
        r = self.parse(v4(UDP))
        self.assertEqual((r['family'], r['len'], r['l4off'], r['proto'], r['ports'], r['sport'], r['dport']),
                         (4, 32, 20, 17, 1, 5004, 40000))
        self.assertEqual((r['saddr'], r['daddr'], r['alen']), (12, 16, 4))

    def test_ipv4_options_move_the_ports(self):
        r = self.parse(v4(UDP, ihl=7))
        self.assertEqual((r['l4off'], r['sport'], r['dport']), (28, 5004, 40000))

    def test_ipv4_lengths_are_checked_against_the_buffer(self):
        self.assertEqual(self.parse(v4(UDP, tot=33), v4(UDP, tot=19), v4(UDP, ihl=4), v4(UDP, ihl=15, tot=40),
                                    v4(UDP)[:19], b'', b'\x45'), [None] * 7)

    def test_ipv4_trailing_bytes_are_not_the_packet(self):
        # the total length, not the buffer, is the packet: what follows is cut off and never read as ports
        r = self.parse(v4(UDP, pad=b'\xff' * 9))
        self.assertEqual(r['len'], 32)
        r = self.parse(v4(b'', pad=UDP))
        self.assertEqual((r['len'], r['ports']), (20, 0))

    def test_ports_only_within_the_packet_and_not_in_a_later_fragment(self):
        self.assertEqual(self.parse(v4(UDP[:3]))['ports'], 0)
        self.assertEqual(self.parse(v4(UDP, frag=0x0002))['ports'], 0)
        self.assertEqual(self.parse(v4(UDP, frag=0x2000))['ports'], 1)       # MF on the first fragment: ports there
        self.assertEqual(self.parse(v4(UDP, proto=1))['ports'], 0)           # ICMP: no ports

    def test_ipv6_udp(self):
        r = self.parse(v6(UDP))
        self.assertEqual((r['family'], r['len'], r['l4off'], r['proto'], r['ports'], r['sport'], r['dport']),
                         (6, 52, 40, 17, 1, 5004, 40000))
        self.assertEqual((r['saddr'], r['daddr'], r['alen']), (8, 24, 16))

    def test_ipv6_lengths_are_checked_against_the_buffer(self):
        self.assertEqual(self.parse(v6(UDP, plen=13), v6(UDP)[:39], v4(UDP, version=6), v6(UDP, version=4),
                                    v6(UDP, version=5)), [None] * 5)
        self.assertEqual(self.parse(v6(UDP, plen=4))['ports'], 1)            # the ports, and no more
        self.assertEqual(self.parse(v6(UDP, plen=3))['ports'], 0)

    def test_ipv6_extension_headers_are_walked_within_the_payload(self):
        hbh = bytes([17, 0]) + b'\x01\x04\0\0\0\0'                           # hop-by-hop, 8 bytes, then UDP
        r = self.parse(v6(hbh + UDP, nh=0))
        self.assertEqual((r['l4off'], r['proto'], r['sport']), (48, 17, 5004))
        dst = bytes([0, 1]) + b'\0' * 14                                     # destination options, 16 bytes
        r = self.parse(v6(dst + hbh + UDP, nh=60))
        self.assertEqual((r['l4off'], r['proto']), (64, 17))
        # an extension header that claims more than there is, or one cut short
        self.assertIsNone(self.parse(v6(bytes([17, 5]) + b'\0' * 6 + UDP, nh=0)))
        self.assertIsNone(self.parse(v6(hbh[:7], nh=0)))
        # a chain longer than any real packet's
        chain = lambda n: v6((bytes([0, 0]) + b'\0' * 6) * (n - 1) + hbh + UDP, nh=0)
        self.assertEqual(self.parse(chain(8))['proto'], 17)
        self.assertIsNone(self.parse(chain(9)))

    def test_ipv6_fragments(self):
        first = bytes([17, 0, 0, 1]) + b'\0\0\0\1'                           # offset 0, M set
        later = bytes([17, 0, 0, 0x11]) + b'\0\0\0\1'                        # offset 2
        r = self.parse(v6(first + UDP, nh=44))
        self.assertEqual((r['proto'], r['ports'], r['sport']), (17, 1, 5004))
        r = self.parse(v6(later + UDP, nh=44))
        self.assertEqual((r['proto'], r['ports']), (17, 0))

    def test_random_packets_stay_within_themselves(self):
        rnd = random.Random(192)
        pkts = []
        for _ in range(3000):
            base = rnd.choice([v4(UDP), v4(UDP, ihl=6), v6(UDP), v6(bytes([17, 0]) + b'\0' * 6 + UDP, nh=0)])
            b = bytearray(base[:rnd.randrange(len(base) + 1)] if rnd.random() < 0.3 else base)
            for _ in range(rnd.randrange(4)):
                if b:
                    b[rnd.randrange(len(b))] = rnd.randrange(256)
            pkts.append(bytes(b))
        for p, r in zip(pkts, self.parse(*pkts)):
            if r is None:
                continue
            self.assertLessEqual(r['len'], len(p))
            self.assertLessEqual(r['l4off'], r['len'])
            self.assertLessEqual(r['daddr'] + r['alen'], r['len'])
            if r['ports']:
                self.assertLessEqual(r['l4off'] + 4, r['len'])


if __name__ == '__main__':
    unittest.main()

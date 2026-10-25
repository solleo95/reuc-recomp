import unittest

from .ip_loss import loss_filter, tcp_payload_size, ip_payload_size


class PacketLossFilterTests(unittest.TestCase):
    def test_only_test_loopback_payload_is_selected(self):
        expression = loss_filter(27889, 1)
        self.assertIn("loopback and ip", expression)
        self.assertIn("ip.SrcAddr == 127.0.0.1 and ip.DstAddr == 127.0.0.1", expression)
        self.assertIn("(tcp.SrcPort == 27889 or tcp.DstPort == 27889)", expression)
        self.assertIn("(udp.SrcPort == 27889 or udp.DstPort == 27889)", expression)
        self.assertNotIn("tcp.PayloadLength", expression)  # ACKs must receive the same network delay.
        self.assertNotIn("random32", expression)

    def test_data_and_ack_classification(self):
        packet = bytearray(40)
        packet[0], packet[9], packet[32] = 0x45, 6, 0x50
        packet[2:4] = len(packet).to_bytes(2, "big")
        self.assertEqual(tcp_payload_size(packet), 0)
        packet.extend(b"input")
        packet[2:4] = len(packet).to_bytes(2, "big")
        self.assertEqual(tcp_payload_size(packet), 5)
        for invalid in (b"", packet[:30], bytes(40)):
            with self.assertRaises(ValueError):
                tcp_payload_size(invalid)

    def test_rejects_unbounded_or_interpolated_filter_arguments(self):
        for port, percent in ((0, 1), (80, 1), (65536, 1), ("27889 or true", 1),
                              (27889, -1), (27889, 0), (27889, 100)):
            with self.subTest(port=port, percent=percent), self.assertRaises(ValueError):
                loss_filter(port, percent)

    def test_udp_packet_validation(self):
        packet = bytearray(28)+b"input"
        packet[0], packet[9] = 0x45, 17
        packet[2:4] = len(packet).to_bytes(2, "big")
        packet[24:26] = (len(packet)-20).to_bytes(2, "big")
        self.assertEqual(ip_payload_size(packet), 5)
        packet[24:26] = (99).to_bytes(2, "big")
        with self.assertRaises(ValueError):
            ip_payload_size(packet)


if __name__ == "__main__":
    unittest.main()

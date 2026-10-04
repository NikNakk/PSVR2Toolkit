"""Synthetic framing/CRC regression checks; contains no proprietary payloads."""
import gzip,struct,tempfile,unittest,zlib
from pathlib import Path
from analyze_psvr2_led_detector_usb import HEADER,MAGIC,iter_records
from analyze_sony_sense_etw_pcap import reports,stream_enhanced_packets


def block(kind,body):
 body+=b'\0'*((-len(body))%4); size=len(body)+12
 return struct.pack('<II',kind,size)+body+struct.pack('<I',size)


def pcap(packet,ts=1234567):
 return (block(0x0a0d0d0a,struct.pack('<IHHq',0x1a2b3c4d,1,0,-1))+
         block(1,struct.pack('<HHI',290,0,65535))+
         block(6,struct.pack('<IIIII',0,ts>>32,ts&0xffffffff,len(packet),len(packet))+packet))


class FramingTests(unittest.TestCase):
 def test_uld_stream_and_truncation(self):
  with tempfile.TemporaryDirectory() as tmp:
   p=Path(tmp)/'if8.bin'; raw=HEADER.pack(MAGIC,1,8,0x89,1234,3)+b'abc'; p.write_bytes(raw*2)
   rs=list(iter_records(p)); self.assertEqual([r.payload for r in rs],[b'abc',b'abc'])
   for bad in [raw[:-1],raw+b'X',HEADER.pack(0,1,8,0x89,1234,0)]:
    p.write_bytes(bad)
    with self.assertRaises(ValueError): list(iter_records(p))
 def test_crc_accept_reject_gzip(self):
  report=bytearray(78); report[0]=0x31; report[21]=2; report[23]=42; report[32:36]=b'\x06\xff\xff\xff'
  report[74:]=struct.pack('<I',zlib.crc32(b'\xa2'+report[:74])&0xffffffff)
  with tempfile.TemporaryDirectory() as tmp:
   p=Path(tmp)/'bt.pcapng.gz'
   with gzip.open(p,'wb') as f: f.write(pcap(b'\xa2'+report))
   rows=list(reports(p)); self.assertEqual(len(rows),1); self.assertEqual(rows[0]['period'],42); self.assertEqual(rows[0]['capture_ts_us'],1234567)
   report[32]^=1
   with gzip.open(p,'wb') as f: f.write(pcap(b'\xa2'+report))
   self.assertEqual(list(reports(p)),[])
 def test_acl_handle_demultiplexing(self):
  report=bytearray(78);report[0]=0x31;report[21]=2;report[23]=42
  report[74:]=struct.pack('<I',zlib.crc32(b'\xa2'+report[:74])&0xffffffff)
  with tempfile.TemporaryDirectory() as tmp:
   p=Path(tmp)/'two.pcapng'
   raw=pcap(struct.pack('<HHHH',12,83,79,66)+b'\xa2'+report)
   raw+=block(6,struct.pack('<IIIII',0,0,1234577,87,87)+struct.pack('<HHHH',13,83,79,66)+b'\xa2'+report)
   p.write_bytes(raw);rows=list(reports(p));self.assertEqual([r['acl_handle'] for r in rows],[12,13])
 def test_pcap_corruption(self):
  with tempfile.TemporaryDirectory() as tmp:
   p=Path(tmp)/'bt.pcapng'; p.write_bytes(pcap(b'hello')[:-1])
   with self.assertRaises(ValueError): list(stream_enhanced_packets(p))
 def test_pcap_timestamp_resolution(self):
  with tempfile.TemporaryDirectory() as tmp:
   p=Path(tmp)/'bt.pcapng'
   raw=block(0x0a0d0d0a,struct.pack('<IHHq',0x1a2b3c4d,1,0,-1))
   raw+=block(1,struct.pack('<HHI',290,0,65535)+struct.pack('<HH',9,1)+b'\x09\0\0\0'+struct.pack('<HH',0,0))
   raw+=block(6,struct.pack('<IIIII',0,0,1234567000,1,1)+b'x'); p.write_bytes(raw)
   self.assertEqual(list(stream_enhanced_packets(p)),[(1234567,b'x')])

if __name__=='__main__': unittest.main()

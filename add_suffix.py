import argparse, sys, struct, binascii

def pid_type(s):
    return int(s, 16)

def _main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--vid', type=pid_type, default='FFFF')
    parser.add_argument('--pid', type=pid_type, default='FFFF')
    parser.add_argument('--bcd', type=pid_type, default='FFFF')
    parser.add_argument('file', type=argparse.FileType('rb'), default='-')
    parser.add_argument('output', type=argparse.FileType('wb'), default='-')
    args = parser.parse_args()

    data = args.file.read()
    args.file.close()

    args.output.write(data)

    suf = struct.pack('<HHHHBBBB', args.bcd, args.pid, args.vid,
        0x100, 0x55, 0x46, 0x44, 0x10)
    args.output.write(suf)

    crc = binascii.crc32(data + suf) & 0xffffffff
    args.output.write(struct.pack('<I', crc))
    args.output.close()

if __name__ == '__main__':
    sys.exit(_main())

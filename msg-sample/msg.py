"""MS-OXMSG writer built on the cfbf module."""
import struct
from datetime import datetime, timezone
from cfbf import Entry, write

# property types
PT_UNICODE = 0x001F
PT_BINARY  = 0x0102
PT_LONG    = 0x0003
PT_BOOLEAN = 0x000B
PT_TIME    = 0x0040

ROOT_CLSID = bytes.fromhex("0b0d020000000000c000000000000046")

# common tags
P_MESSAGE_CLASS   = 0x001A
P_SUBJECT         = 0x0037
P_BODY            = 0x1000
P_SENDER_NAME     = 0x0C1A
P_SENDER_EMAIL    = 0x0C1F
P_SENDER_ADDRTYPE = 0x0C1E
P_DISPLAY_TO      = 0x0E04
P_DISPLAY_CC      = 0x0E03
P_HEADERS         = 0x007D
P_CREATION_TIME   = 0x3007
P_DELIVERY_TIME   = 0x0E06
P_RECIP_TYPE      = 0x0C15
P_DISPLAY_NAME    = 0x3001
P_EMAIL_ADDRESS   = 0x3003
P_ADDRTYPE        = 0x3002
P_ATTACH_FILENAME = 0x3704
P_ATTACH_LONGNAME = 0x3707
P_ATTACH_DATA     = 0x3701
P_ATTACH_METHOD   = 0x3705
P_ATTACH_EXT      = 0x3703
P_ATTACH_NUM      = 0x0E21
P_ROWID           = 0x3000
P_OBJECT_TYPE     = 0x0FFE


def _filetime(dt):
    epoch = datetime(1601, 1, 1, tzinfo=timezone.utc)
    return int((dt - epoch).total_seconds() * 10_000_000)


class PropSet:
    """Accumulates properties, emitting substg streams + the properties stream."""

    def __init__(self):
        self.fixed = []    # (tag, flags, 8-byte value)
        self.streams = {}  # name -> bytes
        self.var = []      # (tag, flags, size)

    def _add_var(self, pid, ptype, raw, size):
        tag = (pid << 16) | ptype
        self.streams["__substg1.0_%08X" % tag] = raw
        self.var.append((tag, 0x00000006, size))

    def unicode(self, pid, value):
        raw = value.encode("utf-16-le")
        # size recorded in the properties stream includes the null terminator
        self._add_var(pid, PT_UNICODE, raw, len(raw) + 2)

    def binary(self, pid, value):
        self._add_var(pid, PT_BINARY, value, len(value))

    def long(self, pid, value):
        self.fixed.append(((pid << 16) | PT_LONG, 0x00000006,
                           struct.pack("<iI", value, 0)))

    def boolean(self, pid, value):
        self.fixed.append(((pid << 16) | PT_BOOLEAN, 0x00000006,
                           struct.pack("<HHI", 1 if value else 0, 0, 0)))

    def time(self, pid, dt):
        self.fixed.append(((pid << 16) | PT_TIME, 0x00000006,
                           struct.pack("<Q", _filetime(dt))))

    def properties_stream(self, header):
        out = bytearray(header)
        for tag, flags, val in self.fixed:
            out += struct.pack("<II", tag, flags) + val
        for tag, flags, size in self.var:
            out += struct.pack("<IIII", tag, flags, size, 0)
        return bytes(out)

    def attach_to(self, storage, header):
        for name, data in self.streams.items():
            storage.add(Entry(name, True, data))
        storage.add(Entry("__properties_version1.0", True,
                          self.properties_stream(header)))


def _nameid(root):
    nid = root.add(Entry("__nameid_version1.0", False))
    for tag in ("00020102", "00030102", "00040102"):
        nid.add(Entry("__substg1.0_" + tag, True, b""))


class Message:
    def __init__(self, message_class="IPM.Note"):
        self.props = PropSet()
        self.props.unicode(P_MESSAGE_CLASS, message_class)
        self.recipients = []
        self.attachments = []

    def add_recipient(self, name, email, kind=1, addrtype="SMTP"):
        """kind: 1=To, 2=Cc, 3=Bcc"""
        p = PropSet()
        p.unicode(P_DISPLAY_NAME, name)
        p.unicode(P_EMAIL_ADDRESS, email)
        p.unicode(P_ADDRTYPE, addrtype)
        p.unicode(0x39FE, email)          # PidTagSmtpAddress
        p.long(P_RECIP_TYPE, kind)
        p.long(P_OBJECT_TYPE, 6)
        p.long(P_ROWID, len(self.recipients))
        self.recipients.append(p)
        return p

    def add_attachment(self, filename, data, ext=None):
        p = PropSet()
        p.unicode(P_ATTACH_FILENAME, filename)
        p.unicode(P_ATTACH_LONGNAME, filename)
        if ext is None and "." in filename:
            ext = filename[filename.rindex("."):]
        if ext:
            p.unicode(P_ATTACH_EXT, ext)
        p.binary(P_ATTACH_DATA, data)
        p.long(P_ATTACH_METHOD, 1)        # by value
        p.long(P_ATTACH_NUM, len(self.attachments))
        p.long(P_OBJECT_TYPE, 7)
        self.attachments.append(p)
        return p

    def save(self, path):
        root = Entry("Root Entry", False, clsid=ROOT_CLSID)
        _nameid(root)

        for i, r in enumerate(self.recipients):
            st = root.add(Entry("__recip_version1.0_#%08X" % i, False))
            r.attach_to(st, struct.pack("<II", 0, 0))

        for i, a in enumerate(self.attachments):
            st = root.add(Entry("__attach_version1.0_#%08X" % i, False))
            a.attach_to(st, struct.pack("<II", 0, 0))

        # top-level header: 8 reserved, next recip id, next attach id,
        # recip count, attach count, 8 reserved
        header = struct.pack("<QIIIIQ", 0,
                             len(self.recipients), len(self.attachments),
                             len(self.recipients), len(self.attachments), 0)
        self.props.attach_to(root, header)
        return write(root, path)

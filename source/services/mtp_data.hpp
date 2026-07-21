#pragma once
// source/services/mtp_data.hpp
// PTP/MTP wire primitives: container framing and dataset marshalling.
//
// Deliberately free of USB and libnx. The transport half (mtp_server) cannot be
// exercised anywhere but real hardware, so everything that *can* be reasoned
// about and unit-tested off-device lives here instead: header parsing, string
// and array encoding, and the DeviceInfo/StorageInfo datasets. Wire format is
// little-endian throughout (PTP over USB).

#include <cstdint>
#include <string>
#include <vector>

namespace Services::Mtp {

// ─── Container types (PTP) ───────────────────────────────────────────────────
namespace Type {
constexpr uint16_t Command  = 1;
constexpr uint16_t Data     = 2;
constexpr uint16_t Response = 3;
constexpr uint16_t Event    = 4;
}

// ─── Operation codes (the subset we answer) ──────────────────────────────────
namespace Op {
constexpr uint16_t GetDeviceInfo      = 0x1001;
constexpr uint16_t OpenSession        = 0x1002;
constexpr uint16_t CloseSession       = 0x1003;
constexpr uint16_t GetStorageIDs      = 0x1004;
constexpr uint16_t GetStorageInfo     = 0x1005;
constexpr uint16_t GetNumObjects      = 0x1006;
constexpr uint16_t GetObjectHandles   = 0x1007;
constexpr uint16_t GetObjectInfo      = 0x1008;
constexpr uint16_t GetObject          = 0x1009;
constexpr uint16_t GetPartialObject   = 0x101B;   // read a byte range (offset,count)
constexpr uint16_t DeleteObject       = 0x100B;
constexpr uint16_t SendObjectInfo     = 0x100C;
constexpr uint16_t SendObject         = 0x100D;
constexpr uint16_t GetDevicePropDesc  = 0x1014;
constexpr uint16_t GetDevicePropValue = 0x1015;

// MTP 1.1 object-property operations (the 0x98xx vendor-extension range).
// SendObjectPropList is the reason these exist here: it is the only way a host
// can declare an incoming object's size in 64 bits. SendObjectInfo's
// ObjectCompressedSize is u32 and reports 0xFFFFFFFF for anything >= 4 GiB,
// which is useless for the containers this app exists to install.
constexpr uint16_t GetObjectPropsSupported = 0x9801;
constexpr uint16_t GetObjectPropDesc       = 0x9802;
constexpr uint16_t GetObjectPropValue      = 0x9803;
constexpr uint16_t SendObjectPropList      = 0x9808;
}

// ─── Device properties ───────────────────────────────────────────────────────
namespace Prop {
constexpr uint16_t DeviceFriendlyName = 0xD402;
}
/// PTP datatype code for a string property.
constexpr uint16_t kTypeStr = 0xFFFF;

// ─── Object property codes ───────────────────────────────────────────────────
namespace ObjProp {
constexpr uint16_t StorageId        = 0xDC01;
constexpr uint16_t ObjectFormat     = 0xDC02;
constexpr uint16_t ProtectionStatus = 0xDC03;
constexpr uint16_t ObjectSize       = 0xDC04;   // UInt64 — the point of all this
constexpr uint16_t ObjectFileName   = 0xDC07;
constexpr uint16_t DateModified     = 0xDC09;
constexpr uint16_t ParentObject     = 0xDC0B;
constexpr uint16_t PersistentUid    = 0xDC41;
constexpr uint16_t Name             = 0xDC44;
}

// ─── PTP datatype codes ──────────────────────────────────────────────────────
// Needed to walk an ObjectPropList: each element's value length is implied by
// its datatype and nothing else, so an unrecognised datatype is unskippable —
// the rest of the dataset becomes unparseable. That is what Invalid_ObjectProp_
// Format exists to report.
namespace DataType {
constexpr uint16_t Int8    = 0x0001;
constexpr uint16_t UInt8   = 0x0002;
constexpr uint16_t Int16   = 0x0003;
constexpr uint16_t UInt16  = 0x0004;
constexpr uint16_t Int32   = 0x0005;
constexpr uint16_t UInt32  = 0x0006;
constexpr uint16_t Int64   = 0x0007;
constexpr uint16_t UInt64  = 0x0008;
constexpr uint16_t Int128  = 0x0009;
constexpr uint16_t UInt128 = 0x000A;
constexpr uint16_t Str     = 0xFFFF;
}

// ─── Object formats ──────────────────────────────────────────────────────────
namespace Fmt {
constexpr uint16_t Undefined   = 0x3000;   // any file
constexpr uint16_t Association = 0x3001;   // a folder
}

/// AssociationType for a folder object.
constexpr uint16_t kAssocGenericFolder = 0x0001;

/// Parent handle meaning "the storage root". Some hosts send 0 instead.
constexpr uint32_t kRootParent = 0xFFFFFFFF;

/// Handle value reserved as "invalid".
constexpr uint32_t kInvalidHandle = 0;

// ─── Response codes ──────────────────────────────────────────────────────────
namespace Rc {
constexpr uint16_t Ok                    = 0x2001;
constexpr uint16_t GeneralError          = 0x2002;
constexpr uint16_t SessionNotOpen        = 0x2003;
constexpr uint16_t InvalidTransactionId  = 0x2004;
constexpr uint16_t OperationNotSupported = 0x2005;
constexpr uint16_t ParameterNotSupported = 0x2006;
constexpr uint16_t IncompleteTransfer    = 0x2007;
constexpr uint16_t InvalidStorageId      = 0x2008;
constexpr uint16_t InvalidObjectHandle   = 0x2009;
constexpr uint16_t StoreFull             = 0x200C;
constexpr uint16_t AccessDenied          = 0x200F;
constexpr uint16_t StoreNotAvailable     = 0x2013;
constexpr uint16_t InvalidParameter      = 0x201D;
constexpr uint16_t SessionAlreadyOpen    = 0x201E;
constexpr uint16_t InvalidParentObject   = 0x201F;

// MTP 1.1 object-property failures.
constexpr uint16_t InvalidObjectPropCode   = 0xA801;
constexpr uint16_t InvalidObjectPropFormat = 0xA802;
constexpr uint16_t InvalidObjectPropValue  = 0xA803;
constexpr uint16_t InvalidDataset          = 0xA806;
constexpr uint16_t ObjectPropNotSupported  = 0xA80A;
}

// ─── Container header ────────────────────────────────────────────────────────
constexpr size_t kHeaderSize = 12;

struct Header {
    uint32_t length = 0;          // total container length incl. this header
    uint16_t type = 0;
    uint16_t code = 0;
    uint32_t transaction_id = 0;
};

/// A command container: header plus up to 5 u32 parameters.
struct Command {
    Header   hdr;
    uint32_t param[5] = {0, 0, 0, 0, 0};
    int      nparams  = 0;
};

bool parse_header(const uint8_t* buf, size_t len, Header& out);
bool parse_command(const uint8_t* buf, size_t len, Command& out);

// ─── Dataset writer ──────────────────────────────────────────────────────────
class Writer {
public:
    void u8(uint8_t v);
    void u16(uint16_t v);
    void u32(uint32_t v);
    void u64(uint64_t v);

    /// MTP string: u8 character count (including the NUL) then UTF-16LE.
    /// An empty string is a single 0 byte, not a lone NUL character.
    void str(const std::string& utf8);

    void au16(const std::vector<uint16_t>& v);   // u32 count, then elements
    void au32(const std::vector<uint32_t>& v);

    void raw(const uint8_t* p, size_t n);

    const std::vector<uint8_t>& data() const { return m_buf; }
    size_t size() const { return m_buf.size(); }

private:
    std::vector<uint8_t> m_buf;
};

// ─── Dataset reader ──────────────────────────────────────────────────────────
class Reader {
public:
    Reader(const uint8_t* p, size_t n) : m_p(p), m_n(n) {}
    bool u8(uint8_t& v);
    bool u16(uint16_t& v);
    bool u32(uint32_t& v);
    bool u64(uint64_t& v);
    bool str(std::string& out);          // UTF-16LE -> UTF-8
    size_t offset() const { return m_i; }
    bool ok() const { return !m_bad; }

private:
    const uint8_t* m_p;
    size_t m_n;
    size_t m_i = 0;
    bool   m_bad = false;
};

// ─── ObjectPropList dataset ──────────────────────────────────────────────────
// The data phase of SendObjectPropList:
//     u32 element_count
//     element[]:
//       u32 object_handle   (0 while the object is still being created)
//       u16 property_code
//       u16 datatype
//       <value>             length implied by datatype
//
// NOTE the size does NOT come from here. SendObjectPropList carries
// ObjectCompressedSize in its COMMAND PARAMETERS, split high/low across
// params 4 and 5 — that split is the actual 64-bit mechanism. A host may also
// include an ObjectSize property in this dataset, but it is not obliged to.
struct ObjectProp {
    uint32_t    handle = 0;
    uint16_t    code   = 0;
    uint16_t    type   = 0;
    uint64_t    num    = 0;   // integer datatypes, zero-extended
    std::string text;         // DataType::Str only
};

/// Parse an ObjectPropList dataset. Returns false if it is malformed,
/// truncated, or contains a datatype whose length we cannot infer. On a
/// datatype failure `out_bad_index` receives the offending element index, which
/// is what the MTP response is expected to carry back to the host.
bool parse_object_prop_list(const uint8_t* p, size_t n,
                            std::vector<ObjectProp>& out,
                            uint32_t* out_bad_index = nullptr);

/// Build an ObjectPropDesc dataset (the data phase of GetObjectPropDesc):
///     u16 property_code
///     u16 datatype
///     u8  get_set          0 = read-only, 1 = host may set it
///     <default_value>      of `datatype`
///     u32 group_code
///     u8  form_flag        0 = none
///     <form>               omitted when form_flag == 0
///
/// Not optional decoration. A host that asks GetObjectPropsSupported what we
/// support then asks GetObjectPropDesc about EACH answer, to learn its datatype
/// before it can encode a value — libmtp gives up on the whole send if any one
/// description fails ("could not get property description"). So this must
/// describe every code GetObjectPropsSupported names, and nothing it names may
/// be undecodable by parse_object_prop_list().
///
/// Returns false for a property we do not support, which the caller reports as
/// ObjectPropNotSupported.
bool build_object_prop_desc(uint16_t code, Writer& w);

/// Find a property by code. Returns nullptr when absent.
const ObjectProp* find_prop(const std::vector<ObjectProp>& props, uint16_t code);

// ─── Container builders ──────────────────────────────────────────────────────
std::vector<uint8_t> make_response(uint16_t code, uint32_t tid,
                                   const uint32_t* params = nullptr, int nparams = 0);
std::vector<uint8_t> make_data(uint16_t code, uint32_t tid,
                               const std::vector<uint8_t>& payload);

// ─── Text conversion (exposed for testing) ───────────────────────────────────
std::vector<uint16_t> utf8_to_utf16(const std::string& s);
std::string           utf16_to_utf8(const std::vector<uint16_t>& s);

} // namespace Services::Mtp

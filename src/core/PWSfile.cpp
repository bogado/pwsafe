/*
 * Copyright (c) 2003-2014 Rony Shapiro <ronys@users.sourceforge.net>.
 * All rights reserved. Use of the code is allowed under the
 * Artistic License 2.0 terms, as specified in the LICENSE file
 * distributed with this code, or available from
 * http://www.opensource.org/licenses/artistic-license-2.0.php
 */
#include "PWSfile.h"
#include "PWSfileV1V2.h"
#include "PWSfileV3.h"
#include "PWSfileV4.h"
#include "SysInfo.h"
#include "core.h"
#include "os/file.h"

#include "sha1.h" // for simple encrypt/decrypt
#include "PWSrand.h" // ditto

#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <limits>

PWSfile *PWSfile::MakePWSfile(const StringX &a_filename, VERSION &version,
                              RWmode mode, int &status,
                              Asker *pAsker, Reporter *pReporter)
{
  if (mode == Read && !pws_os::FileExists(a_filename.c_str())) {
    status = CANT_OPEN_FILE;
    return NULL;
  }

  PWSfile *retval;
  switch (version) {
    case V17:
    case V20:
      status = SUCCESS;
      retval = new PWSfileV1V2(a_filename, mode, version);
      break;
    case V30:
      status = SUCCESS;
      retval = new PWSfileV3(a_filename, mode, version);
      break;
    case V40:
      status = SUCCESS;
      retval = new PWSfileV4(a_filename, mode, version);
      break;
    case UNKNOWN_VERSION:
      ASSERT(mode == Read);
      // XXX Need to rethink this now that V40 doesn't have
      // XXX a cleartext header/footer
      if (PWSfile::ReadVersion(a_filename) == V30) {
        version = V30;
        status = SUCCESS;
        retval = new PWSfileV3(a_filename, mode, version);
      } else {
        version = V20; // may be inaccurate (V17)
        status = SUCCESS;
        retval = new PWSfileV1V2(a_filename, mode, version);
      }
      break;
    default:
      ASSERT(0);
      status = FAILURE; return NULL;
  }
  retval->m_pAsker = pAsker;
  retval->m_pReporter = pReporter;
  return retval;
}


PWSfile::VERSION PWSfile::ReadVersion(const StringX &filename)
{
  if (pws_os::FileExists(filename.c_str())) {
    VERSION v;
    if (PWSfileV3::IsV3x(filename, v))
      return v;
    else
      return V20;
  } else
    return UNKNOWN_VERSION;
}

PWSfile::PWSfile(const StringX &filename, RWmode mode, VERSION v)
  : m_filename(filename), m_passkey(_T("")), m_fd(NULL),
  m_curversion(v), m_rw(mode), m_defusername(_T("")),
  m_fish(NULL), m_terminal(NULL),
  m_nRecordsWithUnknownFields(0)
{
}

PWSfile::~PWSfile()
{
  Close(); // idempotent
}

PWSfile::HeaderRecord::HeaderRecord()
  : m_nCurrentMajorVersion(0), m_nCurrentMinorVersion(0),
    m_file_uuid(pws_os::CUUID::NullUUID()),
    m_prefString(_T("")), m_whenlastsaved(0),
    m_lastsavedby(_T("")), m_lastsavedon(_T("")),
    m_whatlastsaved(_T("")),
    m_dbname(_T("")), m_dbdesc(_T("")), m_yubi_sk(NULL)
{
  m_RUEList.clear();
}

PWSfile::HeaderRecord::HeaderRecord(const PWSfile::HeaderRecord &h) 
  : m_nCurrentMajorVersion(h.m_nCurrentMajorVersion),
    m_nCurrentMinorVersion(h.m_nCurrentMinorVersion),
    m_file_uuid(h.m_file_uuid),
    m_displaystatus(h.m_displaystatus),
    m_prefString(h.m_prefString), m_whenlastsaved(h.m_whenlastsaved),
    m_lastsavedby(h.m_lastsavedby), m_lastsavedon(h.m_lastsavedon),
    m_whatlastsaved(h.m_whatlastsaved),
    m_dbname(h.m_dbname), m_dbdesc(h.m_dbdesc), m_RUEList(h.m_RUEList)
{
  if (h.m_yubi_sk != NULL) {
    m_yubi_sk = new unsigned char[YUBI_SK_LEN];
    memcpy(m_yubi_sk, h.m_yubi_sk, YUBI_SK_LEN);
  } else {
    m_yubi_sk = NULL;
  }
}

PWSfile::HeaderRecord::~HeaderRecord()
{
  if (m_yubi_sk)
    trashMemory(m_yubi_sk, YUBI_SK_LEN);
  delete[] m_yubi_sk;
}

PWSfile::HeaderRecord &PWSfile::HeaderRecord::operator=(const PWSfile::HeaderRecord &h)
{
  if (this != &h) {
    m_nCurrentMajorVersion = h.m_nCurrentMajorVersion;
    m_nCurrentMinorVersion = h.m_nCurrentMinorVersion;
    m_file_uuid = h.m_file_uuid;
    m_displaystatus = h.m_displaystatus;
    m_prefString = h.m_prefString;
    m_whenlastsaved = h.m_whenlastsaved;
    m_lastsavedby = h.m_lastsavedby;
    m_lastsavedon = h.m_lastsavedon;
    m_whatlastsaved = h.m_whatlastsaved;
    m_dbname = h.m_dbname;
    m_dbdesc = h.m_dbdesc;
    m_RUEList = h.m_RUEList;
    if (h.m_yubi_sk != NULL) {
      if (m_yubi_sk)
        trashMemory(m_yubi_sk, YUBI_SK_LEN);
      delete[] m_yubi_sk;
      m_yubi_sk = new unsigned char[YUBI_SK_LEN];
      memcpy(m_yubi_sk, h.m_yubi_sk, YUBI_SK_LEN);
    } else {
      m_yubi_sk = NULL;
    }
  }
  return *this;
}

void PWSfile::FOpen()
{
  ASSERT(!m_filename.empty());
  const TCHAR* m = (m_rw == Read) ? _T("rb") : _T("wb");
  if (m_fd != NULL) {
    fclose(m_fd);
    m_fd = NULL;
  }
  m_fd = pws_os::FOpen(m_filename.c_str(), m);
  m_fileLength = pws_os::fileLength(m_fd);
}

int PWSfile::Close()
{
  delete m_fish;
  m_fish = NULL;
  if (m_fd != NULL) {
    fflush(m_fd);
    fclose(m_fd);
    m_fd = NULL;
  }
  return SUCCESS;
}

size_t PWSfile::WriteCBC(unsigned char type, const unsigned char *data,
                         size_t length)
{
  ASSERT(m_fish != NULL && m_IV != NULL);
  return _writecbc(m_fd, data, length, type, m_fish, m_IV);
}

size_t PWSfile::ReadCBC(unsigned char &type, unsigned char* &data,
                        size_t &length)
{
  unsigned char *buffer = NULL;
  size_t buffer_len = 0;
  size_t retval;

  ASSERT(m_fish != NULL && m_IV != NULL);
  retval = _readcbc(m_fd, buffer, buffer_len, type,
    m_fish, m_IV, m_terminal, m_fileLength);

  if (buffer_len > 0) {
    if (buffer_len < length || data == NULL)
      length = buffer_len; // set to length read
    // if buffer_len > length, data is truncated to length
    // probably an error.
    if (data != NULL) {
      memcpy(data, buffer, length);
      trashMemory(buffer, buffer_len);
      delete[] buffer;
    } else { // NULL data means pass buffer directly to caller
      data = buffer; // caller must trash & delete[]!
    }
  } else {
    // no need to delete[] buffer, since _readcbc will not allocate if
    // buffer_len is zero
  }
  return retval;
}

int PWSfile::CheckPasskey(const StringX &filename,
                          const StringX &passkey, VERSION &version)
{
  /**
   * We start with V3 because it's the quickest to rule out
   * due to header/footer.
   * V4 can take a looong time if the iter value's too big.
   * XXX Need to address this later with a popup prompting the user.
   */
  if (passkey.empty())
    return WRONG_PASSWORD;

  int status;
  version = UNKNOWN_VERSION;
  status = PWSfileV3::CheckPasskey(filename, passkey);
  if (status == SUCCESS) {
    version = V30;
  } else {
    status = PWSfileV4::CheckPasskey(filename, passkey);
    if (status == SUCCESS)
      version = V40;
    else {
      status = PWSfileV1V2::CheckPasskey(filename, passkey);
      if (status == SUCCESS)
        version = V20; // or V17?
    }
  }
  return status;
}

void PWSfile::GetUnknownHeaderFields(UnknownFieldList &UHFL)
{
  if (!m_UHFL.empty())
    UHFL = m_UHFL;
  else
    UHFL.clear();
}

void PWSfile::SetUnknownHeaderFields(UnknownFieldList &UHFL)
{
  if (!UHFL.empty())
    m_UHFL = UHFL;
  else
    m_UHFL.clear();
}

// Following for 'legacy' use of pwsafe as file encryptor/decryptor
// this is for the undocumented 'command line file encryption'
static const stringT CIPHERTEXT_SUFFIX(_S(".PSF"));

static stringT ErrorMessages()
{
  stringT cs_text;

  switch (errno) {
  case EACCES:
    LoadAString(cs_text, IDSC_FILEREADONLY);
    break;
  case EEXIST:
    LoadAString(cs_text, IDSC_FILEEXISTS);
    break;
  case EINVAL:
    LoadAString(cs_text, IDSC_INVALIDFLAG);
    break;
  case EMFILE:
    LoadAString(cs_text, IDSC_NOMOREHANDLES);
    break;
  case ENOENT:
    LoadAString(cs_text, IDSC_FILEPATHNOTFOUND);
    break;
  case EIO: // synthesized upon fwrite failure
    LoadAString(cs_text, IDSC_FILEWRITEERROR);
    break;
  case EFBIG:
    LoadAString(cs_text, IDSC_FILE_TOO_BIG);
    break;
  default:
    break;
  }
  return cs_text;
}

// Following specific for PWSfile::Encrypt
#define SAFE_FWRITE(p, sz, cnt, stream) \
  { \
    size_t _ret = fwrite(p, sz, cnt, stream); \
    if (_ret != cnt) { status = false; goto exit;} \
  }

// std::numeric_limits<>::max() && m'soft's silly macros don't work together
#ifdef max
#undef max
#endif

bool PWSfile::Encrypt(const stringT &fn, const StringX &passwd, stringT &errmess)
{
  ulong64 len = 0;
  size_t slen = 0;
  unsigned char* buf = NULL;
  Fish *fish = NULL;
  bool status = true;
  const stringT out_fn = fn + CIPHERTEXT_SUFFIX;
  unsigned char *pwd = NULL;
  size_t passlen = 0;
  FILE *out = NULL;

  FILE *in = pws_os::FOpen(fn, _T("rb"));
  if (in == NULL) {
    status = false; goto exit;
  }

  len = pws_os::fileLength(in);

  if (len > std::numeric_limits<uint32>::max()) {
    fclose(in);
    errno = EFBIG;
    status = false;
    goto exit;
  }

  slen = static_cast<size_t>(len);
  buf = new unsigned char[slen];

  fread(buf, 1, slen, in);
  if (ferror(in)) { // this is how to detect fread errors
    status = false;
    int save_error = errno;
    fclose(in);
    errno = save_error;
    goto exit;
  }
  if (fclose(in) != 0) {
    status = false;
    goto exit;
  }

  out = pws_os::FOpen(out_fn, _T("wb"));
  if (out == NULL) {
    status = false; goto exit;
  }
  unsigned char randstuff[StuffSize];
  unsigned char randhash[SHA1::HASHLEN];   // HashSize
  PWSrand::GetInstance()->GetRandomData( randstuff, 8 );
  // miserable bug - have to fix this way to avoid breaking existing files
  randstuff[8] = randstuff[9] = TCHAR('\0');
  GenRandhash(passwd, randstuff, randhash);
  SAFE_FWRITE(randstuff, 1,  8, out);
  SAFE_FWRITE(randhash,  1, sizeof(randhash), out);

  unsigned char thesalt[SaltLength];
  PWSrand::GetInstance()->GetRandomData( thesalt, SaltLength );
  SAFE_FWRITE(thesalt, 1, SaltLength, out);

  unsigned char ipthing[8];
  PWSrand::GetInstance()->GetRandomData( ipthing, 8 );
  SAFE_FWRITE(ipthing, 1, 8, out);

  ConvertString(passwd, pwd, passlen);
  fish = BlowFish::MakeBlowFish(pwd, reinterpret_cast<int &>(passlen), thesalt, SaltLength);
  trashMemory(pwd, passlen);
#ifdef UNICODE
  delete[] pwd; // gross - ConvertString allocates only if UNICODE.
#endif
  try {
    _writecbc(out, buf, slen, 0, fish, ipthing);
  } catch (...) { // _writecbc throws an exception if it fails to write
    fclose(out);
    errno = EIO;
    status = false;
    goto exit;
  }
  status = (fclose(out) == 0);
 exit:
  if (!status)
    errmess = ErrorMessages();
  delete fish;
  delete[] buf;
  return status;
}

bool PWSfile::Decrypt(const stringT &fn, const StringX &passwd, stringT &errmess)
{
  ulong64 file_len;
  size_t len;
  unsigned char* buf = NULL;
  bool status = true;
  unsigned char salt[SaltLength];
  unsigned char ipthing[8];
  unsigned char randstuff[StuffSize];
  unsigned char randhash[SHA1::HASHLEN];
  unsigned char temphash[SHA1::HASHLEN];

  FILE *in = pws_os::FOpen(fn, _T("rb"));
  if (in == NULL) {
    status = false;
    goto exit;
  }

  file_len = pws_os::fileLength(in);

  if (file_len < (8 + sizeof(randhash) + 8 + SaltLength)) {
    fclose(in);
    LoadAString(errmess, IDSC_FILE_TOO_SHORT);
    return false;
  }

  fread(randstuff, 1, 8, in);
  randstuff[8] = randstuff[9] = TCHAR('\0'); // ugly bug workaround
  fread(randhash, 1, sizeof(randhash), in);

  GenRandhash(passwd, randstuff, temphash);
  if (memcmp(reinterpret_cast<char *>(randhash), reinterpret_cast<char *>(temphash), SHA1::HASHLEN) != 0) {
    fclose(in);
    LoadAString(errmess, IDSC_BADPASSWORD);
    return false;
  }

  { // decryption in a block, since we use goto
    fread(salt,    1, SaltLength, in);
    fread(ipthing, 1, 8,          in);

    unsigned char dummyType;
    unsigned char *pwd = NULL;
    size_t passlen = 0;
    ConvertString(passwd, pwd, passlen);
    Fish *fish = BlowFish::MakeBlowFish(pwd, reinterpret_cast<int &>(passlen), salt, SaltLength);
    trashMemory(pwd, passlen);
#ifdef UNICODE
    delete[] pwd; // gross - ConvertString allocates only if UNICODE.
#endif
    if (_readcbc(in, buf, len,dummyType, fish, ipthing, 0, file_len) == 0) {
      delete fish;
      delete[] buf; // if not yet allocated, delete[] NULL, which is OK
      return false;
    }
    delete fish;
    fclose(in);
  } // decrypt

  { // write decrypted data
    size_t suffix_len = CIPHERTEXT_SUFFIX.length();
    size_t filepath_len = fn.length();

    stringT out_fn = fn;
    out_fn = out_fn.substr(0,filepath_len - suffix_len);

    FILE *out = pws_os::FOpen(out_fn, _T("wb"));
    if (out != NULL) {
      size_t fret = fwrite(buf, 1, len, out);
      if (fret != len) {
        int save_errno = errno;
        fclose(out);
        errno = save_errno;
        goto exit;
      }
      if (fclose(out) != 0) {
        status = false;
        goto exit;
      }
    } else { // open failed
      status = false;
      goto exit;
    }
  } // write decrypted
 exit:
  if (!status)
    errmess = ErrorMessages();
  delete[] buf; // allocated by _readcbc
  return status;
}

//-----------------------------------------------------------------
// A quick way to determine if two files are equal, or if a given
// file has been modified.

// For large files (>2K), we only hash the head & tail of the file.
// This is due to a performance trade-off.

// For unencrypted file, only checking the head & tail of large files may
// cause changes made to the middle of the file to be missed.
// However, since we write encrypted in CBC mode, this means that if the file
// was modified at offset X, then everything from X to the end of the file will
// be modified and the digests would be different.

PWSFileSig::PWSFileSig(const stringT &fname)
{
  const long THRESHOLD = 2048; // if file's longer than this, hash only head & tail

  m_length = 0;
  m_iErrorCode = PWSfile::SUCCESS;
  memset(m_digest, 0, sizeof(m_digest));
  FILE *fp = pws_os::FOpen(fname, _T("rb"));
  if (fp != NULL) {
    SHA256 hash;
    m_length = pws_os::fileLength(fp);
    // Minimum size for an empty V3 DB is 232 bytes - pre + post, no hdr or records!
    // Probably smaller for V1 & V2 DBs
    if (m_length > 232) {
      unsigned char buf[THRESHOLD];
      if (m_length <= THRESHOLD) {
        if (fread(buf, size_t(m_length), 1, fp) == 1) {
          hash.Update(buf, size_t(m_length));
          hash.Final(m_digest);
        }
      } else { // m_length > THRESHOLD
        if (fread(buf, THRESHOLD / 2, 1, fp) == 1 &&
            fseek(fp, -THRESHOLD / 2, SEEK_END) == 0 &&
            fread(buf + THRESHOLD / 2, THRESHOLD / 2, 1, fp) == 1) {
          hash.Update(buf, THRESHOLD);
          hash.Final(m_digest);
        }
      }
    } else {
      // File too small
      m_iErrorCode = PWSfile::TRUNCATED_FILE;
    }

    fclose(fp);
  } else {
    m_iErrorCode = PWSfile::CANT_OPEN_FILE;
  }
}

PWSFileSig::PWSFileSig(const PWSFileSig &pfs)
{
  m_length = pfs.m_length;
  m_iErrorCode = pfs.m_iErrorCode;
  memcpy(m_digest, pfs.m_digest, sizeof(m_digest));
}

PWSFileSig &PWSFileSig::operator=(const PWSFileSig &that)
{
  if (this != &that) {
    m_length = that.m_length;
    m_iErrorCode = that.m_iErrorCode;
    memcpy(m_digest, that.m_digest, sizeof(m_digest));
  }
  return *this;
}

bool PWSFileSig::operator==(const PWSFileSig &that)
{
  // Check this first as digest may otherwise be invalid
  if (m_iErrorCode != 0 || that.m_iErrorCode != 0)
    return false;

  return (m_length == that.m_length &&
          memcmp(m_digest, that.m_digest, sizeof(m_digest)) == 0);
}

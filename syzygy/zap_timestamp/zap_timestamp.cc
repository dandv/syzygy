// Copyright 2012 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// ZapTimestamps uses PEFile/ImageLayout/BlockGraph to represent a PE file in
// memory, and TypedBlock to navigate through the PE structures of the file.
// We don't use a full decomposition of the image, but rather only decompose
// the PE headers and structures. As such, ZapTimetamps can be seen as a
// lightweight decomposer. It would be better do this directly using the
// internal intermediate representation formats of PEFileParser, but this
// functionality would require some refactoring.
//
// Changes that are required to be made to the PE file are represented by an
// address space, mapping replacement data to file offsets. This address-space
// can then be simply 'stamped' on to the PE file to be modified.
//
// The matching PDB file is completely rewritten to guarantee that it is
// canonical (as long as the underlying PdbWriter doesn't change). We load all
// of the streams into memory, reach in and make local modifications, and
// rewrite the entire file to disk.

#include "syzygy/zap_timestamp/zap_timestamp.h"

#include "base/at_exit.h"
#include "base/bind.h"
#include "base/command_line.h"
#include "base/file_util.h"
#include "base/logging.h"
#include "base/md5.h"
#include "base/files/scoped_temp_dir.h"
#include "base/strings/stringprintf.h"
#include "syzygy/block_graph/typed_block.h"
#include "syzygy/core/file_util.h"
#include "syzygy/pdb/pdb_byte_stream.h"
#include "syzygy/pdb/pdb_constants.h"
#include "syzygy/pdb/pdb_reader.h"
#include "syzygy/pdb/pdb_util.h"
#include "syzygy/pdb/pdb_writer.h"
#include "syzygy/pe/find.h"
#include "syzygy/pe/pdb_info.h"
#include "syzygy/pe/pe_data.h"
#include "syzygy/pe/pe_file_parser.h"
#include "syzygy/pe/pe_file_writer.h"

namespace zap_timestamp {

namespace {

using block_graph::BlockGraph;
using block_graph::ConstTypedBlock;
using core::FileOffsetAddress;
using core::RelativeAddress;
using pdb::PdbByteStream;
using pdb::PdbFile;
using pdb::PdbReader;
using pdb::PdbStream;
using pdb::PdbWriter;
using pdb::WritablePdbStream;
using pe::ImageLayout;
using pe::PEFile;
using pe::PEFileParser;

typedef ConstTypedBlock<IMAGE_DEBUG_DIRECTORY> ImageDebugDirectory;
typedef ConstTypedBlock<IMAGE_DOS_HEADER> DosHeader;
typedef ConstTypedBlock<IMAGE_NT_HEADERS> NtHeaders;
typedef ConstTypedBlock<pe::CvInfoPdb70> CvInfoPdb;
typedef ZapTimestamp::PatchAddressSpace PatchAddressSpace;
typedef ZapTimestamp::PatchData PatchData;

// An intermediate reference type used to track references generated by
// PEFileParser.
struct IntermediateReference {
  BlockGraph::ReferenceType type;
  BlockGraph::Size size;
  RelativeAddress address;
};

// A map of intermediate references. This tracks references created by the
// PEFileParser.
typedef std::map<RelativeAddress, IntermediateReference>
    IntermediateReferenceMap;

// Adds a reference to the given intermediate reference map. Used as a callback
// to PEFileParser.
bool AddReference(IntermediateReferenceMap* references,
                  RelativeAddress source,
                  BlockGraph::ReferenceType type,
                  BlockGraph::Size size,
                  RelativeAddress destination) {
  DCHECK(references != NULL);

  IntermediateReference ref = { type, size, destination };
  return references->insert(std::make_pair(source, ref)).second;
}

// Returns the block and offset into the block associated with the given
// address and size. Sets block pointer to NULL and offset to 0 if no block
// was found for the address and size. Returns true if a block is found, false
// otherwise.
bool LookupBlockOffset(const ImageLayout& image_layout,
                       RelativeAddress address,
                       size_t size,
                       BlockGraph::Block** block,
                       BlockGraph::Offset* offset) {
  DCHECK(block != NULL);
  DCHECK(offset != NULL);

  *block = image_layout.blocks.GetContainingBlock(address, size);
  if (*block == NULL) {
    *offset = 0;
    return false;
  }

  *offset = address - (*block)->addr();
  return true;
}

// Performs a decomposition of the given PE file, only parsing out the PE
// data blocks and references between them.
bool MiniDecompose(const PEFile& pe_file,
                   ImageLayout* image_layout,
                   BlockGraph::Block** dos_header_block) {
  DCHECK(image_layout != NULL);
  DCHECK(dos_header_block != NULL);

  IntermediateReferenceMap references;

  PEFileParser::AddReferenceCallback add_reference = base::Bind(
      &AddReference, base::Unretained(&references));
  PEFileParser pe_file_parser(pe_file, &image_layout->blocks, add_reference);

  PEFileParser::PEHeader pe_header;
  if (!pe_file_parser.ParseImage(&pe_header)) {
    LOG(ERROR) << "Failed to parse PE file: " << pe_file.path().value();
    return false;
  }

  if (!pe::CopyHeaderToImageLayout(pe_header.nt_headers, image_layout)) {
    LOG(ERROR) << "Failed to copy NT headers to image layout.";
    return false;
  }

  // Finalize the intermediate references. We only finalize those that are
  // within the closed set of blocks.
  IntermediateReferenceMap::const_iterator ref_it = references.begin();
  for (; ref_it != references.end(); ++ref_it) {
    BlockGraph::Block* src_block = NULL;
    BlockGraph::Offset src_offset = 0;
    if (!LookupBlockOffset(*image_layout, ref_it->first, ref_it->second.size,
                           &src_block, &src_offset)) {
      continue;
    }

    BlockGraph::Block* dst_block = NULL;
    BlockGraph::Offset dst_offset = 0;
    if (!LookupBlockOffset(*image_layout, ref_it->second.address, 1,
                           &dst_block, &dst_offset)) {
      continue;
    }

    // Make the final reference.
    BlockGraph::Reference ref(ref_it->second.type, ref_it->second.size,
                              dst_block, dst_offset, dst_offset);
    CHECK(src_block->SetReference(src_offset, ref));
  }

  *dos_header_block = pe_header.dos_header;

  return true;
}

// Marks the range of data at @p rel_addr and of size @p size as needing to be
// changed. It will be replaced with the data in @p data, and marked with the
// description @p name (for debugging purposes). The change is recorded in the
// provided PatchAddressSpace @p file_addr_space in terms of file offsets.
// This performs the necessary address space translations via @p pe_file and
// ensures that the change does not conflict with any other required changes.
bool MarkData(const PEFile& pe_file,
              RelativeAddress rel_addr,
              size_t size,
              const uint8* data,
              const base::StringPiece& name,
              PatchAddressSpace* file_addr_space) {
  DCHECK(file_addr_space);

  FileOffsetAddress file_addr;
  if (!pe_file.Translate(rel_addr, &file_addr)) {
    LOG(ERROR) << "Failed to translate " << rel_addr << " to file offset.";
    return false;
  }

  if (!file_addr_space->Insert(PatchAddressSpace::Range(file_addr, size),
                               PatchData(data, name))) {
    LOG(ERROR) << "Failed to insert file range at " << file_addr
               << " of length " << size << ".";
    return false;
  }

  return true;
}

// Given a data directory of type T containing a member variable named
// TimeDateStamp, this will mark the timestamp for changing to the value
// provided in @p timestamp_data. The change will be recorded in the provided
// PatchAddressSpace @p file_addr_space.
template<typename T>
bool MarkDataDirectoryTimestamps(const PEFile& pe_file,
                                 NtHeaders& nt_headers,
                                 size_t data_dir_index,
                                 const char* data_dir_name,
                                 const uint8* timestamp_data,
                                 PatchAddressSpace* file_addr_space) {
  DCHECK_GT(arraysize(nt_headers->OptionalHeader.DataDirectory),
            data_dir_index);
  DCHECK(timestamp_data != NULL);
  DCHECK(file_addr_space != NULL);

  // It is not an error if the debug directory doesn't exist.
  const IMAGE_DATA_DIRECTORY& data_dir_info =
      nt_headers->OptionalHeader.DataDirectory[data_dir_index];
  if (!nt_headers.HasReference(data_dir_info.VirtualAddress)) {
    DCHECK_EQ(0u, data_dir_info.VirtualAddress);
    LOG(INFO) << "PE file contains no data directory " << data_dir_index << ".";
    return true;
  }

  ConstTypedBlock<T> data_dir;
  if (!nt_headers.Dereference(data_dir_info.VirtualAddress, &data_dir)) {
    LOG(ERROR) << "Failed to dereference data directory " << data_dir_index
               << ".";
    return false;
  }

  FileOffsetAddress data_dir_addr;
  if (!pe_file.Translate(data_dir.block()->addr(), &data_dir_addr)) {
    LOG(ERROR) << "Failed to locate data directory " << data_dir_index << ".";
    return false;
  }

  if (data_dir->TimeDateStamp == 0)
    return true;

  FileOffsetAddress timestamp_addr = data_dir_addr +
      data_dir.OffsetOf(data_dir->TimeDateStamp);

  std::string name = base::StringPrintf("%s Timestamp", data_dir_name);
  if (!file_addr_space->Insert(PatchAddressSpace::Range(timestamp_addr,
                                                        sizeof(DWORD)),
                               PatchData(timestamp_data, name))) {
    LOG(ERROR) << "Failed to mark timestamp of data directory "
               << data_dir_index << ".";
    return false;
  }

  return true;
}

bool Md5Consume(size_t bytes, FILE* file, base::MD5Context* context) {
  char buffer[4096] = { 0 };

  size_t cur = 0;
  while (cur < bytes) {
    size_t bytes_to_read = std::min(bytes - cur, sizeof(buffer));
    size_t bytes_read = ::fread(buffer, 1, bytes_to_read, file);
    if (bytes_read != bytes_to_read) {
      LOG(ERROR) << "Error reading from file (got " << bytes_read
                 << ", expected " << bytes_to_read << ").";
      return false;
    }

    base::MD5Update(context, base::StringPiece(buffer, bytes_read));
    cur += bytes_read;
  }
  DCHECK_EQ(cur, bytes);

  return true;
}

bool UpdateFileInPlace(const base::FilePath& path,
                       const PatchAddressSpace& updates) {
  LOG(INFO) << "Patching file: " << path.value();

  base::ScopedFILE file(base::OpenFile(path, "rb+"));
  if (file.get() == NULL) {
    LOG(ERROR) << "Unable to open file for updating: " << path.value();
    return false;
  }

  PatchAddressSpace::const_iterator it = updates.begin();
  for (; it != updates.end(); ++it) {
    // No data? Then nothing to update. This happens for the PE checksum, which
    // has a NULL data pointer. We update it later on in another pass.
    if (it->second.data == NULL)
      continue;

    LOG(INFO) << "  Patching " << it->second.name << ", " << it->first.size()
              << " bytes at " << it->first.start();

    // Seek to the position to be updated.
    if (::fseek(file.get(), it->first.start().value(), SEEK_SET) != 0) {
      LOG(ERROR) << "Failed to seek to " << it->first.start() << " of file: "
                 << path.value();
      return false;
    }

    // Write the updated data.
    size_t bytes_written = ::fwrite(it->second.data, 1, it->first.size(),
                                    file.get());
    if (bytes_written != it->first.size()) {
      LOG(ERROR) << "Failed to write " << it->first.size() << " bytes to "
                 << "position " << it->first.start() << " of file: "
                 << path.value();
    }
  }

  LOG(INFO) << "Finished patching file: " << path.value();
  file.reset();

  return true;
}

// Ensures that the stream with the given ID is writable, returning a scoped
// pointer to it.
scoped_refptr<PdbStream> GetWritablePdbStream(size_t index,
                                              PdbFile* pdb_file) {
  DCHECK(pdb_file != NULL);
  DCHECK_GT(pdb_file->StreamCount(), index);

  scoped_refptr<PdbStream> reader = pdb_file->GetStream(index);

  // Try and get the writer. If it's not available, then replace the stream
  // with a byte stream, which is in-place writable.
  scoped_refptr<WritablePdbStream> writer = reader->GetWritablePdbStream();
  if (writer.get() == NULL) {
    scoped_refptr<PdbByteStream> byte_stream(new PdbByteStream());
    byte_stream->Init(reader);
    pdb_file->ReplaceStream(index, byte_stream);
    reader = byte_stream;
  }

  return reader;
}

void OutputSummaryStats(base::FilePath& path) {
  base::ScopedFILE file(base::OpenFile(path, "rb"));
  if (file.get() == NULL) {
    LOG(ERROR) << "Unable to open file for reading: " << path.value();
    return;
  }
  ::fseek(file.get(), 0, SEEK_END);
  size_t file_size = ::ftell(file.get());
  ::fseek(file.get(), 0, SEEK_SET);

  base::MD5Context md5_context;
  base::MD5Init(&md5_context);
  if (!Md5Consume(file_size, file.get(), &md5_context))
    return;

  base::MD5Digest md5_digest;
  base::MD5Final(&md5_digest, &md5_context);
  std::string md5_string = base::MD5DigestToBase16(md5_digest);

  LOG(INFO) << "Path: " << path.value();
  LOG(INFO) << "  Size  : " << file_size;
  LOG(INFO) << "  Digest: " << md5_string;
}

bool NormalizeDbiStream(DWORD pdb_age_data,
                        PdbByteStream* dbi_stream) {
  DCHECK(dbi_stream != NULL);

  LOG(INFO) << "Updating PDB DBI stream.";

  uint8* dbi_data = dbi_stream->data();
  if (dbi_stream->length() < sizeof(pdb::DbiHeader)) {
    LOG(ERROR) << "DBI stream too short.";
    return false;
  }
  pdb::DbiHeader* dbi_header = reinterpret_cast<pdb::DbiHeader*>(dbi_data);

  // Update the age in the DbiHeader as well. This needs to match pdb_age
  // in the PDB header.
  dbi_header->age = pdb_age_data;
  dbi_data += sizeof(*dbi_header);

  // Ensure that the module information is addressable.
  if (dbi_stream->length() < dbi_header->gp_modi_size) {
    LOG(ERROR) << "Invalid DBI header gp_modi_size.";
    return false;
  }

  // Run over the module information.
  // TODO(chrisha): Use BufferWriter to do this. We need to update it to handle
  //     type casts and bounds checking.
  uint8* module_info_end = dbi_data + dbi_header->gp_modi_size;
  while (dbi_data < module_info_end) {
    pdb::DbiModuleInfoBase* module_info =
        reinterpret_cast<pdb::DbiModuleInfoBase*>(dbi_data);
    module_info->offsets = 0;
    dbi_data += sizeof(*module_info);

    // Skip two NULL terminated strings after the module info.
    while (*dbi_data != 0) ++dbi_data;
    ++dbi_data;
    while (*dbi_data != 0) ++dbi_data;
    ++dbi_data;

    // Skip until we're at a multiple of 4 position.
    size_t offset = dbi_data - dbi_stream->data();
    offset = ((offset + 3) / 4) * 4;
    dbi_data = dbi_stream->data() + offset;
  }

  // Ensure that the section contributions are addressable.
  size_t section_contrib_end_pos = dbi_header->gp_modi_size + sizeof(uint32) +
      dbi_header->section_contribution_size;
  if (dbi_stream->length() < section_contrib_end_pos) {
    LOG(ERROR) << "Invalid DBI header gp_modi_size.";
    return false;
  }

  // Run over the section contributions.
  dbi_data += sizeof(uint32);  // Skip the signature.
  uint8* section_contrib_end = dbi_data + dbi_header->section_contribution_size;
  while (dbi_data < section_contrib_end) {
    pdb::DbiSectionContrib* section_contrib =
        reinterpret_cast<pdb::DbiSectionContrib*>(dbi_data);
    section_contrib->pad1 = 0;
    section_contrib->pad2 = 0;
    dbi_data += sizeof(*section_contrib);
  }

  return true;
}

bool NormalizeSymbolRecordStream(PdbByteStream* stream) {
  DCHECK(stream != NULL);

  uint8* data = stream->data();
  uint8* data_end = data + stream->length();

  while (data < data_end) {
    // Get the size of the symbol record and skip past it.
    uint16* size = reinterpret_cast<uint16*>(data);
    data += sizeof(*size);

    // The size of the symbol record, plus its uint16 length, must be a multiple
    // of 4. Each symbol record consists of the length followed by a symbol
    // type (also a short), so the size needs to be at least of length 2.
    // See http://code.google.com/p/syzygy/wiki/PdbFileFormat for a discussion
    // of the format of this stream.
    DCHECK_LE(2u, *size);
    DCHECK_EQ(0u, ((*size + sizeof(*size)) % 4));

    // Up to the last 3 bytes are padding, as the record gets rounded up to
    // a multiple of 4 in size.
    static const size_t kMaxPadding = 3;
    uint8* end = data + *size;
    uint8* tail = end - kMaxPadding;

    // Skip past the symbol record.
    data = end;

    // Find the null terminator for the record.
    for (; tail + 1 < end && *tail != 0; ++tail) {
      // Intentionally empty.
    }

    // Pad out the rest of the record with nulls (these are usually full of
    // junk bytes).
    for (; tail < end; ++tail)
      *tail = 0;
  }

  return true;
}

}  // namespace

ZapTimestamp::ZapTimestamp()
    : image_layout_(&block_graph_),
      dos_header_block_(NULL),
      write_image_(true),
      write_pdb_(true),
      overwrite_(false) {
  // The timestamp can't just be set to zero as that represents a special
  // value in the PE file. We set it to some arbitrary fixed date in the past.
  // This is Jan 1, 2010, 0:00:00 GMT. This date shouldn't be too much in
  // the past, otherwise Windows might trigger a warning saying that the
  // instrumented image has known incompatibility issues when someone tries to
  // run it.
  timestamp_data_ = 1262304000;

  // Initialize the age to 1.
  pdb_age_data_ = 1;
}

bool ZapTimestamp::Init() {
  if (!ValidatePeAndPdbFiles())
    return false;

  if (!ValidateOutputPaths())
    return false;

  if (!DecomposePeFile())
    return false;

  if (!MarkPeFileRanges())
    return false;

  if (!input_pdb_.empty()) {
    if (!CalculatePdbGuid())
      return false;

    if (!LoadAndUpdatePdbFile())
      return false;
  }

  return true;
}

bool ZapTimestamp::Zap() {
  if (write_image_) {
    if (!WritePeFile())
      return false;
    OutputSummaryStats(input_image_);
  }

  if (!input_pdb_.empty() && write_pdb_) {
    if (!WritePdbFile())
      return false;
    OutputSummaryStats(input_pdb_);
  }

  return true;
}

bool ZapTimestamp::ValidatePeAndPdbFiles() {
  LOG(INFO) << "Analyzing PE file: " << input_image_.value();

  if (!base::PathExists(input_image_) ||
      base::DirectoryExists(input_image_)) {
    LOG(ERROR) << "PE file not found: " << input_image_.value();
    return false;
  }

  if (!pe_file_.Init(input_image_)) {
    LOG(ERROR) << "Failed to read PE file: " << input_image_.value();
    return false;
  }

  if (input_pdb_.empty()) {
    // If the image has no code view entry (ie: no matching PDB file)
    // then accept this fact and leave the PDB path empty.
    pe::PdbInfo pe_pdb_info;
    if (!pe_pdb_info.Init(input_image_))
      return true;

    // Find the matching PDB file.
    if (!pe::FindPdbForModule(input_image_, &input_pdb_)) {
      LOG(ERROR) << "Error while searching for PDB file.";
      return false;
    }
    if (input_pdb_.empty()) {
      LOG(ERROR) << "PDB file not found for PE file: " << input_image_.value();
      return false;
    }
    DCHECK(base::PathExists(input_pdb_));
  } else {
    if (!base::PathExists(input_pdb_) ||
        base::DirectoryExists(input_pdb_)) {
      LOG(ERROR) << "PDB file not found: " << input_pdb_.value();
    }
  }

  // Ensure that the PDB and the PE file are consistent with each other.
  if (!pe::PeAndPdbAreMatched(input_image_, input_pdb_))
    return false;  // This logs verbosely.

  LOG(INFO) << "Found matching PDB file: " << input_pdb_.value();

  return true;
}

bool ZapTimestamp::ValidateOutputPaths() {
  if (output_image_.empty())
    output_image_ = input_image_;

  if (input_pdb_.empty()) {
    if (!output_pdb_.empty()) {
      LOG(INFO) << "Ignoring output-pdb path: " << output_pdb_.value();
      output_pdb_.clear();
    }
  } else {
    if (output_pdb_.empty()) {
      if (input_image_.BaseName() == output_image_.BaseName()) {
        // The input and output have the same basename. Use the input PDB
        // basename, but place it alongside the output image.
        output_pdb_ = output_image_.DirName().Append(input_pdb_.BaseName());
      } else {
        // The basenames don't match. Simpy append ".pdb" to the output
        // image.
        output_pdb_ = output_image_.AddExtension(L"pdb");
      }
    }
  }

  // If overwriting isn't allowed then double check everything is kosher.
  if (!overwrite_) {
    if (write_image_ &&
        (base::PathExists(output_image_) ||
        core::CompareFilePaths(input_image_, output_image_) ==
            core::kEquivalentFilePaths)) {
      LOG(ERROR) << "Output image file exists. Must enable overwrite.";
      return false;
    }
    if (write_pdb_ && !output_pdb_.empty() &&
        (base::PathExists(output_pdb_) ||
        core::CompareFilePaths(input_pdb_, output_pdb_) ==
            core::kEquivalentFilePaths)) {
      LOG(ERROR) << "Output PDB file exists. Must enable overwrite.";
      return false;
    }
  }

  return true;
}

bool ZapTimestamp::DecomposePeFile() {
  // Decompose the image. This is a very high level decomposition only
  // chunking out the PE structures and references from/to PE blocks.
  BlockGraph::Block* dos_header_block = NULL;
  if (!MiniDecompose(pe_file_, &image_layout_, &dos_header_block_))
    return false;

  return true;
}

bool ZapTimestamp::MarkPeFileRanges() {
  DCHECK(dos_header_block_ != NULL);
  LOG(INFO) << "Finding PE fields that need updating.";

  DosHeader dos_header;
  if (!dos_header.Init(0, dos_header_block_)) {
    LOG(ERROR) << "Failed to cast IMAGE_DOS_HEADER.";
    return false;
  }

  NtHeaders nt_headers;
  if (!dos_header.Dereference(dos_header->e_lfanew, &nt_headers)) {
    LOG(ERROR) << "Failed to dereference IMAGE_NT_HEADERS.";
    return false;
  }

  // Mark the export data directory timestamp.
  if (!MarkDataDirectoryTimestamps<IMAGE_EXPORT_DIRECTORY>(
          pe_file_, nt_headers, IMAGE_DIRECTORY_ENTRY_EXPORT,
          "Export Directory",
          reinterpret_cast<const uint8*>(&timestamp_data_),
          &pe_file_addr_space_)) {
    // This logs verbosely on failure.
    return false;
  }

  // Mark the resource data directory timestamp.
  if (!MarkDataDirectoryTimestamps<IMAGE_RESOURCE_DIRECTORY>(
          pe_file_, nt_headers, IMAGE_DIRECTORY_ENTRY_RESOURCE,
          "Resource Directory",
          reinterpret_cast<const uint8*>(&timestamp_data_),
          &pe_file_addr_space_)) {
    // This logs verbosely on failure.
    return false;
  }

  // Find the debug directory.
  ImageDebugDirectory debug_dir;
  const IMAGE_DATA_DIRECTORY& debug_dir_info =
      nt_headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
  if (nt_headers.HasReference(debug_dir_info.VirtualAddress))
    nt_headers.Dereference(debug_dir_info.VirtualAddress, &debug_dir);

  // Within that, find the codeview debug entry. We also update every other
  // debug timestamp.
  CvInfoPdb cv_info_pdb;
  RelativeAddress rel_addr;
  if (debug_dir.block()) {
    for (size_t i = 0; i < debug_dir.ElementCount(); ++i) {
      rel_addr = debug_dir.block()->addr() +
          debug_dir.OffsetOf(debug_dir[i].TimeDateStamp);
      std::string name = base::StringPrintf("Debug Directory %d Timestamp", i);
      if (!MarkData(pe_file_, rel_addr, sizeof(timestamp_data_),
                    reinterpret_cast<const uint8*>(&timestamp_data_), name,
                    &pe_file_addr_space_)) {
        LOG(ERROR) << "Failed to mark TimeDateStamp of debug directory " << i
                   << ".";
        return false;
      }

      if (debug_dir[i].Type == IMAGE_DEBUG_TYPE_CODEVIEW) {
        if (cv_info_pdb.block() != NULL) {
          LOG(ERROR) << "Found multiple CodeView debug directories.";
          return false;
        }
        if (!debug_dir.Dereference(debug_dir[i].PointerToRawData,
                                   &cv_info_pdb)) {
          LOG(ERROR) << "Failed to dereference CodeView debug directory.";
          return false;
        }
      }
    }
  }

  // We should have found a code view debug directory pointing to the PDB file.
  if (!input_pdb_.empty()) {
    if (cv_info_pdb.block() == NULL) {
      LOG(ERROR) << "Failed to find CodeView debug directory.";
      return false;
    }

    // Get the file offset of the PDB age and mark it.
    rel_addr = cv_info_pdb.block()->addr() +
        cv_info_pdb.OffsetOf(cv_info_pdb->pdb_age);
    if (!MarkData(pe_file_, rel_addr, sizeof(pdb_age_data_),
                  reinterpret_cast<const uint8*>(&pdb_age_data_),
                  "PDB Age", &pe_file_addr_space_)) {
      LOG(ERROR) << "Failed to mark PDB age.";
      return false;
    }

    // Get the file offset of the PDB guid and mark it.
    rel_addr = cv_info_pdb.block()->addr() +
        cv_info_pdb.OffsetOf(cv_info_pdb->signature);
    if (!MarkData(pe_file_, rel_addr, sizeof(pdb_guid_data_),
                  reinterpret_cast<const uint8*>(&pdb_guid_data_),
                  "PDB GUID", &pe_file_addr_space_)) {
      LOG(ERROR) << "Failed to mark PDB GUID.";
      return false;
    }
  }

  // Get the file offset of the PE checksum and mark it.
  rel_addr = nt_headers.block()->addr() +
      nt_headers.OffsetOf(nt_headers->OptionalHeader.CheckSum);
  if (!MarkData(pe_file_, rel_addr, sizeof(DWORD), NULL,
                "PE Checksum", &pe_file_addr_space_)) {
    LOG(ERROR) << "Failed to mark PE checksum.";
    return false;
  }

  // Get the file offset of the PE timestamp and mark it.
  rel_addr = nt_headers.block()->addr() +
      nt_headers.OffsetOf(nt_headers->FileHeader.TimeDateStamp);
  if (!MarkData(pe_file_, rel_addr, sizeof(timestamp_data_),
                reinterpret_cast<uint8*>(&timestamp_data_), "PE Timestamp",
                &pe_file_addr_space_)) {
    LOG(ERROR) << "Failed to mark PE timestamp.";
    return false;
  }

  return true;
}

bool ZapTimestamp::CalculatePdbGuid() {
  DCHECK(!input_pdb_.empty());

  LOG(INFO) << "Calculating PDB GUID from PE file contents.";

  base::ScopedFILE pe_file(base::OpenFile(input_image_, "rb"));
  if (pe_file.get() == NULL) {
    LOG(ERROR) << "Failed to open PE file for reading: "
               << input_image_.value();
    return false;
  }

  // Get the length of the entire file.
  if (::fseek(pe_file.get(), 0, SEEK_END) != 0) {
    LOG(ERROR) << "Failed to fseek to end of file.";
    return false;
  }
  FileOffsetAddress end(::ftell(pe_file.get()));

  // Seek back to the beginning.
  if (::fseek(pe_file.get(), 0, SEEK_SET) != 0) {
    LOG(ERROR) << "Failed to fseek to beginning of file.";
    return false;
  }

  // Initialize the MD5 structure.
  base::MD5Context md5_context = { 0 };
  base::MD5Init(&md5_context);

  // We seek through the bits of the file that will be changed, and skip those.
  // The rest of the file (the static parts) are fed through an MD5 hash and
  // used to generated a unique and stable GUID.
  FileOffsetAddress cur(0);
  PatchAddressSpace::const_iterator range_it = pe_file_addr_space_.begin();
  for (; range_it != pe_file_addr_space_.end(); ++range_it) {
    // Consume any data before this range.
    if (cur < range_it->first.start()) {
      size_t bytes_to_hash = range_it->first.start() - cur;
      if (!Md5Consume(bytes_to_hash, pe_file.get(), &md5_context))
        return false;  // This logs verbosely for us.
    }

    if (::fseek(pe_file.get(), range_it->first.size(), SEEK_CUR)) {
      LOG(ERROR) << "Failed to fseek past marked range.";
    }

    cur = range_it->first.end();
  }

  // Consume any left-over data.
  if (cur < end) {
    if (!Md5Consume(end - cur, pe_file.get(), &md5_context))
      return false;  // This logs verbosely for us.
  }

  DCHECK_EQ(end.value(), static_cast<uint32>(::ftell(pe_file.get())));

  COMPILE_ASSERT(sizeof(base::MD5Digest) == sizeof(pdb_guid_data_),
                 md5_digest_and_guid_size_mismatch);
  base::MD5Final(reinterpret_cast<base::MD5Digest*>(&pdb_guid_data_),
                 &md5_context);
  LOG(INFO) << "Final GUID is " << base::MD5DigestToBase16(
      *reinterpret_cast<base::MD5Digest*>(&pdb_guid_data_)) << ".";

  return true;
}

bool ZapTimestamp::LoadAndUpdatePdbFile() {
  DCHECK(!input_pdb_.empty());
  DCHECK(pdb_file_.get() == NULL);

  pdb_file_.reset(new PdbFile());
  PdbReader pdb_reader;
  if (!pdb_reader.Read(input_pdb_, pdb_file_.get())) {
    LOG(ERROR) << "Failed to read PDB file: " << input_pdb_.value();
    return false;
  }

  // We turf the old directory stream as a fresh PDB does not have one. It's
  // also meaningless after we rewrite a PDB as the old blocks it refers to
  // will no longer exist.
  pdb_file_->ReplaceStream(pdb::kPdbOldDirectoryStream, NULL);

  scoped_refptr<PdbStream> header_reader =
      GetWritablePdbStream(pdb::kPdbHeaderInfoStream, pdb_file_.get());
  if (header_reader.get() == NULL) {
    LOG(ERROR) << "No header info stream in PDB file: " << input_pdb_.value();
    return false;
  }

  scoped_refptr<WritablePdbStream> header_writer =
      header_reader->GetWritablePdbStream();
  DCHECK(header_writer.get() != NULL);

  // Update the timestamp, the age and the signature.
  LOG(INFO) << "Updating PDB header.";
  header_writer->set_pos(offsetof(pdb::PdbInfoHeader70, timestamp));
  header_writer->Write(static_cast<uint32>(timestamp_data_));
  header_writer->Write(static_cast<uint32>(pdb_age_data_));
  header_writer->Write(pdb_guid_data_);

  // Normalize the DBI stream in place.
  scoped_refptr<PdbByteStream> dbi_stream(new PdbByteStream());
  CHECK(dbi_stream->Init(pdb_file_->GetStream(pdb::kDbiStream)));
  pdb_file_->ReplaceStream(pdb::kDbiStream, dbi_stream);
  if (!NormalizeDbiStream(pdb_age_data_, dbi_stream)) {
    LOG(ERROR) << "Failed to normalize DBI stream.";
    return false;
  }

  uint8* dbi_data = dbi_stream->data();
  pdb::DbiHeader* dbi_header = reinterpret_cast<pdb::DbiHeader*>(dbi_data);

  // Normalize the symbol record stream in place.
  scoped_refptr<PdbByteStream> symrec_stream(new PdbByteStream());
  CHECK(symrec_stream->Init(pdb_file_->GetStream(
      dbi_header->symbol_record_stream)));
  pdb_file_->ReplaceStream(dbi_header->symbol_record_stream, symrec_stream);
  if (!NormalizeSymbolRecordStream(symrec_stream)) {
    LOG(ERROR) << "Failed to normalize symbol record stream.";
    return false;
  }

  // Normalize the public symbol info stream. There's a DWORD of padding at
  // offset 24 that we want to zero.
  scoped_refptr<PdbStream> pubsym_reader = GetWritablePdbStream(
      dbi_header->public_symbol_info_stream, pdb_file_.get());
  scoped_refptr<WritablePdbStream> pubsym_writer =
      pubsym_reader->GetWritablePdbStream();
  DCHECK(pubsym_writer.get() != NULL);
  pubsym_writer->set_pos(24);
  pubsym_writer->Write(static_cast<uint32>(0));

  return true;
}

bool ZapTimestamp::WritePeFile() {
  if (core::CompareFilePaths(input_image_, output_image_) !=
          core::kEquivalentFilePaths) {
    if (::CopyFileW(input_image_.value().c_str(),
                    output_image_.value().c_str(),
                    FALSE) == FALSE) {
      LOG(ERROR) << "Failed to write output image: %s"
                 << output_image_.value();
      return false;
    }
  }

  if (!UpdateFileInPlace(output_image_, pe_file_addr_space_))
    return false;

  LOG(INFO) << "Updating checksum for PE file: " << output_image_.value();
  if (!pe::PEFileWriter::UpdateFileChecksum(output_image_)) {
    LOG(ERROR) << "Failed to update checksum for PE file: "
               << output_image_.value();
    return false;
  }

  return true;
}

bool ZapTimestamp::WritePdbFile() {
  DCHECK(!input_pdb_.empty());

  // We actually completely rewrite the PDB file to a temporary location, and
  // then move it over top of the existing one. This is because pdb_file_
  // actually has an open file handle to the original PDB.

  // We create a temporary directory alongside the final destination so as
  // not to cross volume boundaries.
  base::FilePath output_dir = output_pdb_.DirName();
  base::ScopedTempDir temp_dir;
  if (!temp_dir.CreateUniqueTempDirUnderPath(output_dir)) {
    LOG(ERROR) << "Failed to create temporary directory in \""
               << output_dir.value() << "\".";
    return false;
  }

  // Generate the path to the rewritten PDB.
  base::FilePath temp_path = temp_dir.path().Append(input_pdb_.BaseName());

  PdbWriter pdb_writer;
  LOG(INFO) << "Creating temporary PDB file: " << temp_path.value();
  if (!pdb_writer.Write(temp_path, *pdb_file_.get())) {
    LOG(ERROR) << "Failed to write new PDB: " << temp_path.value();
    return false;
  }

  // Free up the PDB file. This will close the open file handle to the original
  // PDB file.
  pdb_file_.reset(NULL);

  // Copy over top of the original file.
  LOG(INFO) << "Temporary PDB file replacing destination PDB: "
            << output_pdb_.value();
  base::File::Error error;
  if (!base::ReplaceFileW(temp_path, output_pdb_, &error)) {
    LOG(ERROR) << "Unable to replace PDB file.";
    return false;
  }

  return true;
}

}  // namespace zap_timestamp

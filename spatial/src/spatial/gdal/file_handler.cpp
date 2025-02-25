#include "spatial/gdal/file_handler.hpp"

#include "duckdb/common/mutex.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/common/types/uuid.hpp"

#include "cpl_vsi.h"
#include "cpl_vsi_virtual.h"
#include "cpl_vsi_error.h"
#include "cpl_string.h"

namespace spatial {

namespace gdal {

//--------------------------------------------------------------------------
// GDAL DuckDB File handle wrapper
//--------------------------------------------------------------------------

class DuckDBFileHandle : public VSIVirtualHandle {
private:
	unique_ptr<FileHandle> file_handle;
public:
	explicit DuckDBFileHandle(unique_ptr<FileHandle> file_handle_p)
	    : file_handle(std::move(file_handle_p))
	{ }

	vsi_l_offset Tell() override {
		return static_cast<vsi_l_offset>(file_handle->SeekPosition());
	}
	int Seek(vsi_l_offset nOffset, int nWhence) override {
		switch (nWhence) {
		case SEEK_SET:
			file_handle->Seek(nOffset);
			break;
		case SEEK_CUR:
			file_handle->Seek(file_handle->SeekPosition() + nOffset);
			break;
		case SEEK_END:
			file_handle->Seek(file_handle->GetFileSize() + nOffset);
			break;
		default:
			throw InternalException("Unknown seek type");
		}
		return 0;
	}

	size_t Read(void *pBuffer, size_t nSize, size_t nCount) override {
		auto remaining_bytes = nSize * nCount;
		while(remaining_bytes > 0) {
			auto read_bytes = file_handle->Read(pBuffer, remaining_bytes);
			if(read_bytes == 0) {
				break;
			}
			remaining_bytes -= read_bytes;
			// Note we performed a cast back to void*
			pBuffer = static_cast<uint8_t*>(pBuffer) + read_bytes;
		}
		return nCount - (remaining_bytes / nSize);
	}

	int Eof() override {
		return file_handle->SeekPosition() == file_handle->GetFileSize() ? TRUE : FALSE;
	}


	size_t Write(const void *pBuffer, size_t nSize, size_t nCount) override {
		auto written_bytes = file_handle->Write(const_cast<void *>(pBuffer), nSize * nCount);
		// Return the number of items written
		return static_cast<size_t>(written_bytes / nSize);
	}

	int Flush() override {
		file_handle->Sync();
		return 0;
	}
	int Truncate(vsi_l_offset nNewSize) override {
		file_handle->Truncate(static_cast<int64_t>(nNewSize));
		return 0;
	}
	int Close() override {
		file_handle->Close();
		return 0;
	}

	// int ReadMultiRange(int nRanges, void **ppData, const vsi_l_offset *panOffsets, const size_t *panSizes) override;
	// void AdviseRead(int nRanges, const vsi_l_offset *panOffsets, const size_t *panSizes) override;
	// VSIRangeStatus GetRangeStatus(vsi_l_offset nOffset, vsi_l_offset nLength) override;
};


//--------------------------------------------------------------------------
// GDAL DuckDB File system wrapper
//--------------------------------------------------------------------------
class DuckDBFileSystemHandler : public VSIFilesystemHandler {
private:
	string client_prefix;
	ClientContext& context;
public:
	DuckDBFileSystemHandler(string client_prefix, ClientContext& context)
	    : client_prefix(std::move(client_prefix)), context(context) { };

	const char* StripPrefix(const char *pszFilename)
	{
		return pszFilename + client_prefix.size();
	}

	VSIVirtualHandle *Open(const char *prefixed_file_name, const char *access,
	                       bool bSetError, CSLConstList /* papszOptions */) override {
		auto file_name = StripPrefix(prefixed_file_name);
		auto &fs = FileSystem::GetFileSystem(context);

		// TODO: Double check that this is correct
		uint8_t flags;
		auto len = strlen(access);
		if (access[0] == 'r') {
			flags = FileFlags::FILE_FLAGS_READ;
			if (len > 1 && access[1] == '+') {
				flags |= FileFlags::FILE_FLAGS_WRITE;
			}
			if (len > 2 && access[2] == '+') {
				// might be "rb+"
				flags |= FileFlags::FILE_FLAGS_WRITE;
			}
		} else if (access[0] == 'w') {
			flags = FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE_NEW;
			if (len > 1 && access[1] == '+') {
				flags |= FileFlags::FILE_FLAGS_READ;
			}
			if (len > 2 && access[2] == '+') {
				// might be "wb+"
				flags |= FileFlags::FILE_FLAGS_READ;
			}
		} else if (access[0] == 'a') {
			flags = FileFlags::FILE_FLAGS_APPEND;
			if (len > 1 && access[1] == '+') {
				flags |= FileFlags::FILE_FLAGS_READ;
			}
			if (len > 2 && access[2] == '+') {
				// might be "ab+"
				flags |= FileFlags::FILE_FLAGS_READ;
			}
		} else {
			throw InternalException("Unknown file access type");
		}

		try {
			string path(file_name);
			auto file = fs.OpenFile(file_name, flags);
			return new DuckDBFileHandle(std::move(file));
		} catch (std::exception &ex) {
			if(bSetError) {
				VSIError(VSIE_FileError, "Failed to open file %s: %s", file_name, ex.what());
			}
			return nullptr;
		}
	}

	int Stat(const char *prefixed_file_name, VSIStatBufL *pstatbuf, int n_flags) override {
		auto file_name = StripPrefix(prefixed_file_name);
		auto &fs = FileSystem::GetFileSystem(context);

		memset(pstatbuf, 0, sizeof(VSIStatBufL));

		unique_ptr<FileHandle> file;
		try {
			file = fs.OpenFile(file_name, FileFlags::FILE_FLAGS_READ);
		} catch (std::exception &ex) {
			return -1;
		}

		if(!file) {
			return -1;
		}

		pstatbuf->st_size = static_cast<off_t>(fs.GetFileSize(*file));
		pstatbuf->st_mtime = fs.GetLastModifiedTime(*file);

		auto type = file->GetType();
		switch (type) {
		// These are the only three types present on all platforms
		case FileType::FILE_TYPE_REGULAR:
			pstatbuf->st_mode = S_IFREG;
			break;
		case FileType::FILE_TYPE_DIR:
			pstatbuf->st_mode = S_IFDIR;
			break;
		case FileType::FILE_TYPE_CHARDEV:
			pstatbuf->st_mode = S_IFCHR;
			break;
		default:
			// HTTPFS returns invalid type for everything basically.
			if(FileSystem::IsRemoteFile(file_name)) {
				pstatbuf->st_mode = S_IFREG;
			}
			else {
				return -1;
			}
		}

		return 0;
	}

	int Mkdir(const char *prefixed_dir_name, long mode) override {
		auto dir_name = StripPrefix(prefixed_dir_name);
		auto &fs = FileSystem::GetFileSystem(context);

		fs.CreateDirectory(dir_name);
		return 0;
	}

	int Rmdir(const char *prefixed_dir_name) override {
		auto dir_name = StripPrefix(prefixed_dir_name);
		auto &fs = FileSystem::GetFileSystem(context);

		fs.RemoveDirectory(dir_name);
		return 0;
	}

	int RmdirRecursive(const char *prefixed_dir_name) override {
		auto dir_name = StripPrefix(prefixed_dir_name);
		auto &fs = FileSystem::GetFileSystem(context);

		fs.RemoveDirectory(dir_name);
		return 0;
	}

	char **ReadDirEx(const char *prefixed_dir_name, int max_files) override {
		auto dir_name = StripPrefix(prefixed_dir_name);
		auto &fs = FileSystem::GetFileSystem(context);

		CPLStringList files;
		auto files_count = 0;
		fs.ListFiles(dir_name, [&](const string &file_name, bool is_dir) {
			if (files_count >= max_files) {
				return;
			}
			files.AddString(file_name.c_str());
			files_count++;
		});
		return files.StealList();
	}

	char **SiblingFiles(const char *prefixed_file_name) override {
		auto file_name = StripPrefix(prefixed_file_name);

		auto &fs = FileSystem::GetFileSystem(context);
		CPLStringList files;
		auto file_vector = fs.Glob(file_name);
		for (auto &file : file_vector) {
			files.AddString(file.c_str());
		}
		return files.StealList();
	}

	int HasOptimizedReadMultiRange(const char *pszPath) override {
		return 0;
	}

	int Unlink(const char *prefixed_file_name) override {
		auto file_name = StripPrefix(prefixed_file_name);
		auto &fs = FileSystem::GetFileSystem(context);
		try {
			fs.RemoveFile(file_name);
			return 0;
		} catch (std::exception &ex) {
			return -1;
		}
	}
};


//--------------------------------------------------------------------------
// GDALClientContextState
//--------------------------------------------------------------------------
//
// We give every client a unique prefix so that multiple connections can
// use their own attached file systems. This is necessary because GDAL is
// not otherwise aware of the connection context.
//
GDALClientContextState::GDALClientContextState(ClientContext& context) {

	// Create a new random prefix for this client
	client_prefix = StringUtil::Format("/vsiduckdb-%s/", UUID::ToString(UUID::GenerateRandomUUID()));

	// Create a new file handler responding to this prefix
	fs_handler = new DuckDBFileSystemHandler(client_prefix, context);

	// Register the file handler
	VSIFileManager::InstallHandler(client_prefix, fs_handler);
}

GDALClientContextState::~GDALClientContextState() {
	// Uninstall the file handler for this prefix
	VSIFileManager::RemoveHandler(client_prefix);

	// Delete the file handler
	delete fs_handler;
}

void GDALClientContextState::QueryEnd(){

};

const string& GDALClientContextState::GetPrefix() const {
	return client_prefix;
}

GDALClientContextState& GDALClientContextState::GetOrCreate(ClientContext& context) {
	if(!context.registered_state["gdal"]) {
		context.registered_state["gdal"] = make_uniq<GDALClientContextState>(context);
	}
	return *dynamic_cast<GDALClientContextState*>(context.registered_state["gdal"].get());
}

} // namespace gdal

} // namespace spatial

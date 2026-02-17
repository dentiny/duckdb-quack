#pragma once

namespace duckdb {

class FileSystem;

class SslKeyGenerator {
public:
	static void GenerateSslKeys(const std::string &cert_filename, const std::string &private_key_filename,
	                            const std::string &dh_filename, size_t cert_days_valid);

	static std::string GetDefaultCertificateDirectory(FileSystem &fs);
};
} // namespace duckdb

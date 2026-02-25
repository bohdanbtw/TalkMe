#pragma once
#include <string>
#include <cstddef>
#include <vector>

namespace TalkMe {

	std::string GenerateBase32Secret(size_t length = 16);
	bool VerifyTOTP(const std::string& base32Secret, const std::string& userCode);

} // namespace TalkMe
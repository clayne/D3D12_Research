#pragma once

namespace Utils
{
	struct ForceFunctionToBeLinked
	{
		ForceFunctionToBeLinked(const void* p) { SetLastError(PtrToInt(p)); }
	};

	inline std::string GetTimeString()
	{
		SYSTEMTIME time;
		GetSystemTime(&time);
		return Sprintf("%d_%02d_%02d__%02d_%02d_%02d_%d",
			time.wYear, time.wMonth, time.wDay,
			time.wHour, time.wMinute, time.wSecond, time.wMilliseconds);
	}
}

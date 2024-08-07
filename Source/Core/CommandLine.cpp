#include "stdafx.h"
#include "CommandLine.h"

static HashMap<StringHash, String> m_Parameters;
static String m_CommandLine;

bool CommandLine::Parse(const char* pCommandLine)
{
	m_CommandLine = pCommandLine;
	m_Parameters.clear();
	bool quoted = false;

	int commandStart = 0;
	bool hasValue = false;
	String identifier;

	for (size_t i = 0; i < m_CommandLine.size(); i++)
	{
		if (m_CommandLine[i] == '\"')
		{
			quoted = !quoted;
		}
		else if (m_CommandLine[i] == '-' && !quoted)
		{
			commandStart = (int)i + 1;
		}
		else if (m_CommandLine[i] == '=' && !quoted)
		{
			identifier = m_CommandLine.substr(commandStart, i - commandStart);
			commandStart = (int)i + 1;
			hasValue = true;
		}
		else if (m_CommandLine[i] == ' ' && !quoted)
		{
			if (hasValue)
			{
				String value = m_CommandLine.substr(commandStart, i - commandStart);
				if (value.front() == '\"' && value.back() == '\"')
				{
					value = value.substr(1, value.length() - 2);
				}

				m_Parameters[StringHash(identifier)] = value;
				hasValue = false;
			}
			else
			{
				m_Parameters[StringHash(m_CommandLine.substr(commandStart, i - commandStart))] = "1";
			}
			commandStart = -1;
		}
	}

	if (commandStart > -1)
	{
		if (hasValue)
		{
			String value = m_CommandLine.substr(commandStart);
			if (value.front() == '\"' && value.back() == '\"')
			{
				value = value.substr(1, value.length() - 2);
			}

			m_Parameters[StringHash(identifier)] = value;
			hasValue = false;
		}
		else
		{
			m_Parameters[StringHash(m_CommandLine.substr(commandStart))] = "1";
		}
		commandStart = -1;
	}

	return true;
}

bool CommandLine::GetInt(const char* name, int& value, int defaultValue /*= 0*/)
{
	const char* pValue;
	if (GetValue(name, &pValue))
	{
		value = std::stoi(pValue);
		return true;
	}
	value = defaultValue;
	return false;
}

bool CommandLine::GetBool(const char* parameter)
{
	const char* pValue;
	return GetValue(parameter, &pValue);
}

bool CommandLine::GetValue(const char* pName, const char** pOutValue)
{
	auto it = m_Parameters.find(pName);
	if (it != m_Parameters.end())
	{
		*pOutValue = it->second.c_str();
		return true;
	}
	return false;
}

const String& CommandLine::Get()
{
	return m_CommandLine;
}

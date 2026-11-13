/*
 *   Copyright (c) 2023 by Thomas A. Early N7TAE
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include "CurlGet.h"

CCurlGet::CCurlGet()
{
	curl_global_init(CURL_GLOBAL_ALL);
}

CCurlGet::~CCurlGet()
{
	curl_global_cleanup();
}

// callback function writes data to a std::ostream
size_t CCurlGet::data_write(void* buf, size_t size, size_t nmemb, void* userp)
{
	if(userp)
	{
		std::ostream& os = *static_cast<std::ostream*>(userp);
		std::streamsize len = size * nmemb;
		if(os.write(static_cast<char*>(buf), len))
			return len;
	}

	return 0;
}

CURLcode CCurlGet::GetURL(const std::string &url, std::stringstream &ss, long timeout)
{
	CURLcode code(CURLE_FAILED_INIT);
	CURL* curl = curl_easy_init();

	if(curl)
	{
		if(CURLE_OK == (code = curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &data_write))
		&& CURLE_OK == (code = curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L))
		&& CURLE_OK == (code = curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L))
		&& CURLE_OK == (code = curl_easy_setopt(curl, CURLOPT_FILE, &ss))
		&& CURLE_OK == (code = curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout))
		&& CURLE_OK == (code = curl_easy_setopt(curl, CURLOPT_URL, url.c_str())))
		{
			code = curl_easy_perform(curl);
		}
		curl_easy_cleanup(curl);
	}
	if (code != CURLE_OK)
	{
		std::cout << "WARNING: was not able retrieve data at '" << url << "'\nCurl returned: " << code << std::endl;
	}
	return code;
}

CURLcode CCurlGet::PostForm(const std::string &url, const std::string &field, const std::string &value, std::stringstream &ss, long timeout)
{
	CURLcode code(CURLE_FAILED_INIT);
	CURL* curl = curl_easy_init();

	if (curl)
	{
		// curl owns the escaped string, so it has to outlive the perform().
		char *escaped = curl_easy_escape(curl, value.c_str(), (int)value.size());
		if (escaped)
		{
			const std::string body(field + "=" + escaped);
			if(CURLE_OK == (code = curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &data_write))
			&& CURLE_OK == (code = curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L))
			&& CURLE_OK == (code = curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L))
			&& CURLE_OK == (code = curl_easy_setopt(curl, CURLOPT_FILE, &ss))
			&& CURLE_OK == (code = curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout))
			&& CURLE_OK == (code = curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str()))
			&& CURLE_OK == (code = curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size()))
			&& CURLE_OK == (code = curl_easy_setopt(curl, CURLOPT_URL, url.c_str())))
			{
				// Content-Type defaults to application/x-www-form-urlencoded,
				// which is what the receiving service expects.
				code = curl_easy_perform(curl);
				if (CURLE_OK == code)
				{
					// A reachable server that answers with an error is still a
					// failed registration.
					long status = 0;
					curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
					if (status >= 400)
					{
						std::cout << "WARNING: " << url << " answered HTTP " << status << std::endl;
						code = CURLE_HTTP_RETURNED_ERROR;
					}
				}
			}
			curl_free(escaped);
		}
		curl_easy_cleanup(curl);
	}
	return code;
}

/*
 Copyright 2019 Alain Dargelas

 Licensed under the Apache License, Version 2.0 (the "License");
 you may not use this file except in compliance with the License.
 You may obtain a copy of the License at

 http://www.apache.org/licenses/LICENSE-2.0

 Unless required by applicable law or agreed to in writing, software
 distributed under the License is distributed on an "AS IS" BASIS,
 WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 See the License for the specific language governing permissions and
 limitations under the License.
 */

/*
 * File:   ParseCache.h
 * Author: alain
 *
 * Created on April 29, 2017, 4:20 PM
 */

#ifndef SURELOG_PARSECACHE_H
#define SURELOG_PARSECACHE_H
#pragma once

#include <Surelog/Cache/Cache.h>

namespace SURELOG {

class ParseFile;

class ParseCache : Cache {
 public:
  ParseCache(ParseFile* pp);

  bool restore();
  bool save();
  bool isValid();

 private:
  ParseCache(const ParseCache& orig) = delete;

  PathId getCacheFileId_(PathId ppFileId) const;
  bool restore_(PathId cacheFileId, const std::vector<char>& content);
  bool checkCacheIsValid_(PathId cacheFileId) const;
  bool checkCacheIsValid_(PathId cacheFileId,
                          const std::vector<char>& content) const;

  ParseFile* const m_parse = nullptr;
};

}  // namespace SURELOG

#endif /* SURELOG_PARSECACHE_H */

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
 * File:   Enum.h
 * Author: alain
 *
 * Created on May 19, 2019, 11:55 AM
 */

#ifndef SURELOG_ENUM_H
#define SURELOG_ENUM_H
#pragma once

#include <Surelog/Common/SymbolId.h>
#include <Surelog/Design/DataType.h>

// UHDM
#include <uhdm/uhdm_forward_decl.h>

#include <map>
#include <string>

namespace SURELOG {

class FileContent;
class Value;

class Enum : public DataType {
  SURELOG_IMPLEMENT_RTTI(Enum, DataType)
 public:
  Enum(const FileContent* fC, NodeId nameId, NodeId baseTypeId);
  ~Enum() override = default;

  typedef std::map<std::string, std::pair<unsigned int, Value*>> NameValueMap;

  void addValue(const std::string& name, unsigned int lineNb, Value* value) {
    m_values.emplace(name, std::make_pair(lineNb, value));
  }
  Value* getValue(const std::string& name) const;
  NodeId getDefinitionId() const { return m_nameId; }
  NameValueMap& getValues() { return m_values; }

  UHDM::typespec* getBaseTypespec() const { return m_baseTypespec; }
  void setBaseTypespec(UHDM::typespec* typespec) { m_baseTypespec = typespec; }

 private:
  const NodeId m_nameId;
  NameValueMap m_values;
  UHDM::typespec* m_baseTypespec;
};

}  // namespace SURELOG

#endif /* SURELOG_ENUM_H */

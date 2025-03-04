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
 * File:   Scope.h
 * Author: alain
 *
 * Created on August 31, 2019, 11:24 AM
 */

#ifndef SURELOG_SCOPE_H
#define SURELOG_SCOPE_H
#pragma once

#include <Surelog/Common/RTTI.h>

#include <string>
#include <map>
#include <vector>

namespace SURELOG {

class DataType;
class Statement;
class Variable;

class Scope : public RTTI {
  SURELOG_IMPLEMENT_RTTI(Scope, RTTI)
 public:
  typedef std::map<std::string, Variable*> VariableMap;
  typedef std::map<std::string, DataType*> DataTypeMap;
  typedef std::vector<Statement*> StmtVector;
  typedef std::vector<Scope*> ScopeVector;

  Scope(const std::string& name, Scope* parent)
      : m_name(name), m_parentScope(parent) {}
  ~Scope() override {}

  const std::string& getName() const { return m_name; }
  Scope* getParentScope() { return m_parentScope; }

  void addVariable(Variable* var);

  VariableMap& getVariables() { return m_variables; }
  Variable* getVariable(const std::string& name);

  DataTypeMap& getUsedDataTypeMap() { return m_usedDataTypes; }
  DataType* getUsedDataType(const std::string& name);
  void insertUsedDataType(const std::string& dataTypeName, DataType* dataType) {
    m_usedDataTypes.emplace(dataTypeName, dataType);
  }

  void addStmt(Statement* stmt) { m_statements.push_back(stmt); }
  StmtVector& getStmts() { return m_statements; }

  void addScope(Scope* scope) { m_scopes.push_back(scope); }
  ScopeVector& getScopes() { return m_scopes; }

 private:
  const std::string m_name;
  Scope* const m_parentScope;
  VariableMap m_variables;
  DataTypeMap m_usedDataTypes;
  StmtVector m_statements;
  ScopeVector m_scopes;
};

}  // namespace SURELOG
SURELOG_IMPLEMENT_RTTI_VIRTUAL_CAST_FUNCTIONS(scope_cast, SURELOG::Scope)

#endif /* SURELOG_SCOPE_H */

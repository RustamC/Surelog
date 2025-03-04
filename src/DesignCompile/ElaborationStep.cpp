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
 * File:   ElaborationStep.cpp
 * Author: alain
 *
 * Created on July 12, 2017, 8:55 PM
 */

#include <Surelog/CommandLine/CommandLineParser.h>
#include <Surelog/Common/FileSystem.h>
#include <Surelog/Design/DataType.h>
#include <Surelog/Design/DummyType.h>
#include <Surelog/Design/Enum.h>
#include <Surelog/Design/FileContent.h>
#include <Surelog/Design/Function.h>
#include <Surelog/Design/ModuleDefinition.h>
#include <Surelog/Design/ModuleInstance.h>
#include <Surelog/Design/Netlist.h>
#include <Surelog/Design/Parameter.h>
#include <Surelog/Design/SimpleType.h>
#include <Surelog/Design/Struct.h>
#include <Surelog/Design/TfPortItem.h>
#include <Surelog/Design/Union.h>
#include <Surelog/DesignCompile/CompileDesign.h>
#include <Surelog/DesignCompile/ElaborationStep.h>
#include <Surelog/Library/Library.h>
#include <Surelog/Package/Package.h>
#include <Surelog/SourceCompile/Compiler.h>
#include <Surelog/SourceCompile/SymbolTable.h>
#include <Surelog/Testbench/ClassDefinition.h>
#include <Surelog/Testbench/Program.h>
#include <Surelog/Testbench/Property.h>
#include <Surelog/Testbench/TypeDef.h>
#include <Surelog/Utils/StringUtils.h>

#include <cstring>
#include <queue>

// UHDM
#include <uhdm/ElaboratorListener.h>
#include <uhdm/ExprEval.h>
#include <uhdm/clone_tree.h>
#include <uhdm/uhdm.h>

namespace SURELOG {

using namespace UHDM;  // NOLINT (using a bunch of these)

ElaborationStep::ElaborationStep(CompileDesign* compileDesign)
    : m_compileDesign(compileDesign) {
  m_exprBuilder.seterrorReporting(
      m_compileDesign->getCompiler()->getErrorContainer(),
      m_compileDesign->getCompiler()->getSymbolTable());
  m_exprBuilder.setDesign(m_compileDesign->getCompiler()->getDesign());
  m_helper.seterrorReporting(
      m_compileDesign->getCompiler()->getErrorContainer(),
      m_compileDesign->getCompiler()->getSymbolTable());
  m_symbols = m_compileDesign->getCompiler()->getSymbolTable();
  m_errors = m_compileDesign->getCompiler()->getErrorContainer();
}

ElaborationStep::~ElaborationStep() {}

bool ElaborationStep::bindTypedefs_() {
  FileSystem* const fileSystem = FileSystem::getInstance();
  Compiler* compiler = m_compileDesign->getCompiler();
  ErrorContainer* errors = compiler->getErrorContainer();
  SymbolTable* symbols = compiler->getSymbolTable();
  Design* design = compiler->getDesign();
  Serializer& s = m_compileDesign->getSerializer();
  std::vector<std::pair<TypeDef*, DesignComponent*>> defs;
  std::map<std::string, typespec*> specs;
  for (const auto& file : design->getAllFileContents()) {
    FileContent* fC = file.second;
    for (const auto& typed : fC->getTypeDefMap()) {
      TypeDef* typd = typed.second;
      defs.emplace_back(typd, fC);
    }
  }

  for (const auto& package : design->getPackageDefinitions()) {
    Package* pack = package.second;
    for (const auto& typed : pack->getTypeDefMap()) {
      TypeDef* typd = typed.second;
      defs.emplace_back(typd, pack);
    }
  }

  for (const auto& module : design->getModuleDefinitions()) {
    ModuleDefinition* mod = module.second;
    for (const auto& typed : mod->getTypeDefMap()) {
      TypeDef* typd = typed.second;
      defs.emplace_back(typd, mod);
    }
  }

  for (const auto& program_def : design->getProgramDefinitions()) {
    Program* program = program_def.second;
    for (const auto& typed : program->getTypeDefMap()) {
      TypeDef* typd = typed.second;
      defs.emplace_back(typd, program);
    }
  }

  for (const auto& class_def : design->getClassDefinitions()) {
    ClassDefinition* classp = class_def.second;
    for (const auto& typed : classp->getTypeDefMap()) {
      TypeDef* typd = typed.second;
      defs.emplace_back(typd, classp);
    }
  }

  for (auto& defTuple : defs) {
    TypeDef* typd = defTuple.first;
    DesignComponent* comp = defTuple.second;
    const DataType* prevDef = typd->getDefinition();
    bool noTypespec = false;
    if (prevDef) {
      prevDef = prevDef->getActual();
      if (prevDef->getTypespec() == nullptr)
        noTypespec = true;
      else {
        specs.emplace(prevDef->getTypespec()->VpiName(),
                      prevDef->getTypespec());
        if (Package* pack = valuedcomponenti_cast<Package*>(comp)) {
          std::string name =
              StrCat(pack->getName(), "::", prevDef->getTypespec()->VpiName());
          specs.emplace(name, prevDef->getTypespec());
        }
        if (ClassDefinition* pack =
                valuedcomponenti_cast<ClassDefinition*>(comp)) {
          std::string name =
              StrCat(pack->getName(), "::", prevDef->getTypespec()->VpiName());
          specs.emplace(name, prevDef->getTypespec());
        }
      }
    }

    if (noTypespec == true) {
      if (prevDef && prevDef->getCategory() == DataType::Category::DUMMY) {
        const DataType* def =
            bindTypeDef_(typd, comp, ErrorDefinition::NO_ERROR_MESSAGE);
        if (def && (typd != def)) {
          typd->setDefinition(def);
          typd->setDataType((DataType*)def);
          NodeId id = typd->getDefinitionNode();
          const FileContent* fC = typd->getFileContent();
          NodeId Packed_dimension = fC->Sibling(id);
          typespec* tpclone = nullptr;
          if (Packed_dimension &&
              fC->Type(Packed_dimension) == VObjectType::slPacked_dimension) {
            tpclone = m_helper.compileTypespec(
                defTuple.second, typd->getFileContent(),
                typd->getDefinitionNode(), m_compileDesign, nullptr, nullptr,
                true);
          } else if (typespec* tps = def->getTypespec()) {
            ElaboratorListener listener(&s, false, true);
            tpclone = (typespec*)UHDM::clone_tree((any*)tps, s, &listener);
            tpclone->Typedef_alias(tps);
          }
          if (typespec* unpacked = prevDef->getUnpackedTypespec()) {
            ElaboratorListener listener(&s, false, true);
            array_typespec* unpacked_clone =
                (array_typespec*)UHDM::clone_tree((any*)unpacked, s, &listener);
            unpacked_clone->Elem_typespec(tpclone);
            tpclone = unpacked_clone;
          }

          if (tpclone) {
            typd->setTypespec(tpclone);
            tpclone->VpiName(typd->getName());
            specs.emplace(typd->getName(), tpclone);
            if (Package* pack = valuedcomponenti_cast<Package*>(comp)) {
              std::string name = StrCat(pack->getName(), "::", typd->getName());
              specs.emplace(name, tpclone);
            }
          }
        }
      }
      if (typd->getTypespec() == nullptr) {
        const FileContent* typeF = typd->getFileContent();
        NodeId typeId = typd->getDefinitionNode();
        UHDM::typespec* ts =
            m_helper.compileTypespec(defTuple.second, typeF, typeId,
                                     m_compileDesign, nullptr, nullptr, true);
        if (ts) {
          ts->VpiName(typd->getName());
          std::string name;
          if (typeF->Type(typeId) == VObjectType::slStringConst) {
            name = typeF->SymName(typeId);
          } else {
            name = typd->getName();
          }
          specs.emplace(typd->getName(), ts);
          if (Package* pack = valuedcomponenti_cast<Package*>(comp)) {
            std::string name = StrCat(pack->getName(), "::", typd->getName());
            specs.emplace(name, ts);
          }
          if (ClassDefinition* pack =
                  valuedcomponenti_cast<ClassDefinition*>(comp)) {
            std::string name = StrCat(pack->getName(), "::", typd->getName());
            specs.emplace(name, ts);
          }
          if (ts->UhdmType() == uhdmunsupported_typespec) {
            Location loc1(fileSystem->toPathId(ts->VpiFile(), symbols),
                          ts->VpiLineNo(), ts->VpiColumnNo(),
                          symbols->registerSymbol(name));
            Error err1(ErrorDefinition::COMP_UNDEFINED_TYPE, loc1);
            errors->addError(err1);
          }
        }
        typd->setTypespec(ts);
        if (DataType* dt = (DataType*)typd->getDataType()) {
          if (dt->getTypespec() == nullptr) {
            dt->setTypespec(ts);
          }
        }
      }
    } else if (prevDef == nullptr) {
      const DataType* def =
          bindTypeDef_(typd, comp, ErrorDefinition::NO_ERROR_MESSAGE);
      if (def && (typd != def)) {
        typd->setDefinition(def);
        typd->setDataType((DataType*)def);
        typd->setTypespec(nullptr);
        UHDM::typespec* ts = m_helper.compileTypespec(
            defTuple.second, typd->getFileContent(), typd->getDefinitionNode(),
            m_compileDesign, nullptr, nullptr, true);
        if (ts) {
          specs.emplace(typd->getName(), ts);
          ts->VpiName(typd->getName());
          if (Package* pack = valuedcomponenti_cast<Package*>(comp)) {
            std::string name = StrCat(pack->getName(), "::", typd->getName());
            specs.emplace(name, ts);
          }
        }
        typd->setTypespec(ts);
      } else {
        if (prevDef == nullptr) {
          const FileContent* fC = typd->getFileContent();
          NodeId id = typd->getNodeId();
          std::string definition_string;
          NodeId defNode = typd->getDefinitionNode();
          VObjectType defType = fC->Type(defNode);
          if (defType == VObjectType::slStringConst) {
            definition_string = fC->SymName(defNode);
          }
          Location loc1(fC->getFileId(id), fC->Line(id), fC->Column(id),
                        symbols->registerSymbol(definition_string));
          Error err1(ErrorDefinition::COMP_UNDEFINED_TYPE, loc1);
          errors->addError(err1);
        }
      }
    }
    if (typespec* tps = typd->getTypespec()) {
      for (any* var : comp->getLateTypedefBinding()) {
        const typespec* orig = nullptr;
        if (expr* ex = any_cast<expr*>(var)) {
          orig = ex->Typespec();
        } else if (typespec_member* ex = any_cast<typespec_member*>(var)) {
          orig = ex->Typespec();
        } else if (parameter* ex = any_cast<parameter*>(var)) {
          orig = ex->Typespec();
        } else if (type_parameter* ex = any_cast<type_parameter*>(var)) {
          orig = ex->Typespec();
        } else if (io_decl* ex = any_cast<io_decl*>(var)) {
          orig = ex->Typespec();
        }
        if (orig && (orig->UhdmType() == uhdmunsupported_typespec)) {
          const std::string& need = orig->VpiName();
          if (need == tps->VpiName()) {
            s.unsupported_typespecMaker.Erase((unsupported_typespec*)orig);
            if (expr* ex = any_cast<expr*>(var)) {
              ex->Typespec(tps);
            } else if (typespec_member* ex = any_cast<typespec_member*>(var)) {
              ex->Typespec(tps);
            } else if (parameter* ex = any_cast<parameter*>(var)) {
              ex->Typespec(tps);
            } else if (type_parameter* ex = any_cast<type_parameter*>(var)) {
              ex->Typespec(tps);
            } else if (io_decl* ex = any_cast<io_decl*>(var)) {
              ex->Typespec(tps);
            }
          }
        }
      }
    }
  }
  for (const auto& module : design->getPackageDefinitions()) {
    Package* pack = module.second;
    std::vector<Package*> packages;
    packages.push_back(pack);
    packages.push_back(pack->getUnElabPackage());
    for (auto comp : packages) {
      for (any* var : comp->getLateTypedefBinding()) {
        const typespec* orig = nullptr;
        if (expr* ex = any_cast<expr*>(var)) {
          orig = ex->Typespec();
        } else if (typespec_member* ex = any_cast<typespec_member*>(var)) {
          orig = ex->Typespec();
        } else if (parameter* ex = any_cast<parameter*>(var)) {
          orig = ex->Typespec();
        } else if (type_parameter* ex = any_cast<type_parameter*>(var)) {
          orig = ex->Typespec();
        } else if (io_decl* ex = any_cast<io_decl*>(var)) {
          orig = ex->Typespec();
        }
        if (orig && (orig->UhdmType() == uhdmunsupported_typespec)) {
          const std::string& need = orig->VpiName();
          std::map<std::string, typespec*>::iterator itr = specs.find(need);
          if (itr != specs.end()) {
            typespec* tps = (*itr).second;
            s.unsupported_typespecMaker.Erase((unsupported_typespec*)orig);
            if (expr* ex = any_cast<expr*>(var)) {
              ex->Typespec(tps);
            } else if (typespec_member* ex = any_cast<typespec_member*>(var)) {
              ex->Typespec(tps);
            } else if (parameter* ex = any_cast<parameter*>(var)) {
              ex->Typespec(tps);
            } else if (type_parameter* ex = any_cast<type_parameter*>(var)) {
              ex->Typespec(tps);
            } else if (io_decl* ex = any_cast<io_decl*>(var)) {
              ex->Typespec(tps);
            }
          }
        }
      }
    }
  }
  for (const auto& module : design->getModuleDefinitions()) {
    DesignComponent* comp = module.second;
    for (any* var : comp->getLateTypedefBinding()) {
      const typespec* orig = nullptr;
      if (expr* ex = any_cast<expr*>(var)) {
        orig = ex->Typespec();
      } else if (typespec_member* ex = any_cast<typespec_member*>(var)) {
        orig = ex->Typespec();
      } else if (parameter* ex = any_cast<parameter*>(var)) {
        orig = ex->Typespec();
      } else if (type_parameter* ex = any_cast<type_parameter*>(var)) {
        orig = ex->Typespec();
      } else if (io_decl* ex = any_cast<io_decl*>(var)) {
        orig = ex->Typespec();
      }
      if (orig && (orig->UhdmType() == uhdmunsupported_typespec)) {
        const std::string& need = orig->VpiName();
        std::map<std::string, typespec*>::iterator itr = specs.find(need);

        if (itr != specs.end()) {
          typespec* tps = (*itr).second;
          s.unsupported_typespecMaker.Erase((unsupported_typespec*)orig);
          if (expr* ex = any_cast<expr*>(var)) {
            ex->Typespec(tps);
          } else if (typespec_member* ex = any_cast<typespec_member*>(var)) {
            ex->Typespec(tps);
          } else if (parameter* ex = any_cast<parameter*>(var)) {
            ex->Typespec(tps);
          } else if (type_parameter* ex = any_cast<type_parameter*>(var)) {
            ex->Typespec(tps);
          } else if (io_decl* ex = any_cast<io_decl*>(var)) {
            ex->Typespec(tps);
          }
        }
      }
    }
  }
  for (const auto& module : design->getClassDefinitions()) {
    DesignComponent* comp = module.second;
    for (any* var : comp->getLateTypedefBinding()) {
      const typespec* orig = nullptr;
      if (expr* ex = any_cast<expr*>(var)) {
        orig = ex->Typespec();
      } else if (typespec_member* ex = any_cast<typespec_member*>(var)) {
        orig = ex->Typespec();
      } else if (parameter* ex = any_cast<parameter*>(var)) {
        orig = ex->Typespec();
      } else if (type_parameter* ex = any_cast<type_parameter*>(var)) {
        orig = ex->Typespec();
      } else if (io_decl* ex = any_cast<io_decl*>(var)) {
        orig = ex->Typespec();
      }
      if (orig && (orig->UhdmType() == uhdmunsupported_typespec)) {
        const std::string& need = orig->VpiName();
        std::map<std::string, typespec*>::iterator itr = specs.find(need);
        if (itr != specs.end()) {
          typespec* tps = (*itr).second;
          s.unsupported_typespecMaker.Erase((unsupported_typespec*)orig);
          if (expr* ex = any_cast<expr*>(var)) {
            ex->Typespec(tps);
          } else if (typespec_member* ex = any_cast<typespec_member*>(var)) {
            ex->Typespec(tps);
          } else if (parameter* ex = any_cast<parameter*>(var)) {
            ex->Typespec(tps);
          } else if (type_parameter* ex = any_cast<type_parameter*>(var)) {
            ex->Typespec(tps);
          } else if (io_decl* ex = any_cast<io_decl*>(var)) {
            ex->Typespec(tps);
          }
        }
      }
    }
  }
  return true;
}

bool ElaborationStep::bindTypedefsPostElab_() {
  Compiler* compiler = m_compileDesign->getCompiler();
  Design* design = compiler->getDesign();
  std::queue<ModuleInstance*> queue;
  for (auto instance : design->getTopLevelModuleInstances()) {
    queue.push(instance);
  }

  while (!queue.empty()) {
    ModuleInstance* current = queue.front();
    queue.pop();
    if (current == nullptr) continue;
    for (unsigned int i = 0; i < current->getNbChildren(); i++) {
      queue.push(current->getChildren(i));
    }
    if (auto comp = current->getDefinition()) {
      for (any* var : comp->getLateTypedefBinding()) {
        const typespec* orig = nullptr;
        if (expr* ex = any_cast<expr*>(var)) {
          orig = ex->Typespec();
        } else if (typespec_member* ex = any_cast<typespec_member*>(var)) {
          orig = ex->Typespec();
        } else if (parameter* ex = any_cast<parameter*>(var)) {
          orig = ex->Typespec();
        } else if (type_parameter* ex = any_cast<type_parameter*>(var)) {
          orig = ex->Typespec();
        } else if (io_decl* ex = any_cast<io_decl*>(var)) {
          orig = ex->Typespec();
        }
        if (orig && (orig->UhdmType() == uhdmunsupported_typespec)) {
          const std::string& need = orig->VpiName();
          if (Netlist* netlist = current->getNetlist()) {
            typespec* tps = nullptr;
            bool found = false;
            if (netlist->nets()) {
              for (auto net : *netlist->nets()) {
                if (net->VpiName() == need) {
                  tps = (typespec*)net->Typespec();
                  found = true;
                  break;
                }
              }
            }
            if (tps == nullptr) {
              if (netlist->variables()) {
                for (auto var : *netlist->variables()) {
                  if (var->VpiName() == need) {
                    tps = (typespec*)var->Typespec();
                    found = true;
                    break;
                  }
                }
              }
            }
            if (found == true) {
              if (expr* ex = any_cast<expr*>(var)) {
                ex->Typespec(tps);
              } else if (typespec_member* ex =
                             any_cast<typespec_member*>(var)) {
                ex->Typespec(tps);
              } else if (parameter* ex = any_cast<parameter*>(var)) {
                ex->Typespec(tps);
              } else if (type_parameter* ex = any_cast<type_parameter*>(var)) {
                ex->Typespec(tps);
              } else if (io_decl* ex = any_cast<io_decl*>(var)) {
                ex->Typespec(tps);
              }
            }
          }
        }
      }
    }
  }

  return true;
}

const DataType* ElaborationStep::bindTypeDef_(
    TypeDef* typd, const DesignComponent* parent,
    ErrorDefinition::ErrorType errtype) {
  Compiler* compiler = m_compileDesign->getCompiler();
  SymbolTable* symbols = compiler->getSymbolTable();
  NodeId defNode = typd->getDefinitionNode();
  const FileContent* fC = typd->getFileContent();
  VObjectType defType = fC->Type(defNode);
  std::string objName;
  if (defType == VObjectType::slStringConst) {
    objName = fC->SymName(defNode);
  } else if (defType == VObjectType::slClass_scope) {
    NodeId class_type = fC->Child(defNode);
    NodeId nameId = fC->Child(class_type);
    objName = fC->SymName(nameId) + "::" + fC->SymName(fC->Sibling(defNode));
  } else {
    objName = "NOT_A_VALID_TYPE_NAME";
    symbols->registerSymbol(objName);
  }

  const DataType* result = bindDataType_(objName, fC, defNode, parent, errtype);
  if (result != typd)
    return result;
  else
    return nullptr;
}

const DataType* ElaborationStep::bindDataType_(
    const std::string& type_name, const FileContent* fC, NodeId id,
    const DesignComponent* parent, ErrorDefinition::ErrorType errtype) {
  Compiler* compiler = m_compileDesign->getCompiler();
  ErrorContainer* errors = compiler->getErrorContainer();
  SymbolTable* symbols = compiler->getSymbolTable();
  Design* design = compiler->getDesign();
  std::string libName = "work";
  if (!parent->getFileContents().empty()) {
    libName = parent->getFileContents()[0]->getLibrary()->getName();
  }
  ClassNameClassDefinitionMultiMap classes = design->getClassDefinitions();
  bool found = false;
  bool classFound = false;
  std::string class_in_lib = libName + "@" + type_name;
  ClassNameClassDefinitionMultiMap::iterator itr1 = classes.end();

  if (type_name == "signed") {
    return new DataType(fC, id, type_name, VObjectType::slSigning_Signed);
  } else if (type_name == "unsigned") {
    return new DataType(fC, id, type_name, VObjectType::slSigning_Unsigned);
  } else if (type_name == "logic") {
    return new DataType(fC, id, type_name, VObjectType::slIntVec_TypeLogic);
  } else if (type_name == "bit") {
    return new DataType(fC, id, type_name, VObjectType::slIntVec_TypeBit);
  } else if (type_name == "byte") {
    return new DataType(fC, id, type_name, VObjectType::slIntegerAtomType_Byte);
  }

  const DataType* result = nullptr;
  if ((result = parent->getDataType(type_name))) {
    found = true;
  }
  if (found == false) {
    itr1 = classes.find(class_in_lib);

    if (itr1 != classes.end()) {
      found = true;
      classFound = true;
    }
  }
  if (found == false) {
    itr1 = classes.find(type_name);

    if (itr1 != classes.end()) {
      found = true;
      classFound = true;
    }
  }
  if (found == false) {
    std::string class_in_class = StrCat(parent->getName(), "::", type_name);
    itr1 = classes.find(class_in_class);

    if (itr1 != classes.end()) {
      found = true;
      classFound = true;
    }
  }
  if (found == false) {
    if (parent->getParentScope()) {
      std::string class_in_own_package =
          StrCat(((DesignComponent*)parent->getParentScope())->getName(),
                 "::", type_name);
      itr1 = classes.find(class_in_own_package);
      if (itr1 != classes.end()) {
        found = true;
        classFound = true;
      }
    }
  }
  if (found == false) {
    for (const auto& package : parent->getAccessPackages()) {
      std::string class_in_package =
          StrCat(package->getName(), "::", type_name);
      itr1 = classes.find(class_in_package);
      if (itr1 != classes.end()) {
        found = true;
        classFound = true;
        break;
      }
      const DataType* dtype = package->getDataType(type_name);
      if (dtype) {
        found = true;
        result = dtype;
        break;
      }
    }
  }
  if (found == false) {
    const ClassDefinition* classDefinition =
        valuedcomponenti_cast<const ClassDefinition*>(parent);
    if (classDefinition) {
      if (classDefinition->getName() == type_name) {
        result = classDefinition;
        found = true;
      }
      if (found == false) {
        Parameter* param = classDefinition->getParameter(type_name);
        if (param) {
          found = true;
          result = param;
        }
      }
      if (found == false) {
        if ((result = classDefinition->getBaseDataType(type_name))) {
          found = true;
        }
      }
      if (found == false) {
        if (classDefinition->getContainer()) {
          const DataType* dtype =
              classDefinition->getContainer()->getDataType(type_name);
          if (dtype) {
            found = true;
            result = dtype;
          }
        }
      }
    }
  }
  if (found == false) {
    const TypeDef* def = parent->getTypeDef(type_name);
    if (def) {
      found = true;
      result = def;
    }
  }

  if (found == false) {
    auto res = parent->getNamedObject(type_name);
    if (res) {
      DesignComponent* comp = res->second;
      result = valuedcomponenti_cast<ClassDefinition*>(comp);
      if (result) found = true;
    }
  }
  if (found == false) {
    auto res = parent->getNamedObject(libName + "@" + type_name);
    if (res) {
      DesignComponent* comp = res->second;
      result = valuedcomponenti_cast<ClassDefinition*>(comp);
      if (result) found = true;
    }
  }
  if (found == false) {
    if (type_name.find("::") != std::string::npos) {
      std::vector<std::string_view> args;
      StringUtils::tokenizeMulti(type_name, "::", args);
      std::string_view classOrPackageName = args[0];
      std::string_view the_type_name = args[1];
      itr1 = classes.find(StrCat(libName, "@", classOrPackageName));
      if (itr1 == classes.end()) {
        if (parent->getParentScope()) {
          std::string class_in_own_package =
              StrCat(((DesignComponent*)parent->getParentScope())->getName(),
                     "::", classOrPackageName);
          itr1 = classes.find(class_in_own_package);
        }
      }
      if (itr1 != classes.end()) {
        const DataType* dtype = (*itr1).second->getDataType(the_type_name);
        if (dtype) {
          result = dtype;
          found = true;
        }
      }
      if (found == false) {
        Package* pack = design->getPackage(classOrPackageName);
        if (pack) {
          const DataType* dtype = pack->getDataType(the_type_name);
          if (dtype) {
            result = dtype;
            found = true;
          }
          if (found == false) {
            dtype = pack->getDataType(type_name);
            if (dtype) {
              result = dtype;
              found = true;
            }
          }
          if (found == false) {
            dtype = pack->getClassDefinition(type_name);
            if (dtype) {
              result = dtype;
              found = true;
            }
          }
        }
      }
    }
  }

  if ((found == false) && (errtype != ErrorDefinition::NO_ERROR_MESSAGE)) {
    Location loc1(fC->getFileId(id), fC->Line(id), fC->Column(id),
                  symbols->registerSymbol(type_name));
    Location loc2(symbols->registerSymbol(parent->getName()));
    Error err1(errtype, loc1, loc2);
    errors->addError(err1);
  } else {
    if (classFound == true) {
      // Binding
      ClassDefinition* def = (*itr1).second;
      result = def;
    }
  }
  while (result && result->getDefinition()) {
    result = result->getDefinition();
  }

  return result;
}

Variable* ElaborationStep::bindVariable_(const std::string& var_name,
                                         Scope* scope, const FileContent* fC,
                                         NodeId id,
                                         const DesignComponent* parent,
                                         ErrorDefinition::ErrorType errtype,
                                         bool returnClassParam) {
  Compiler* compiler = m_compileDesign->getCompiler();
  ErrorContainer* errors = compiler->getErrorContainer();
  SymbolTable* symbols = compiler->getSymbolTable();
  Variable* result = nullptr;

  const ClassDefinition* classDefinition =
      valuedcomponenti_cast<const ClassDefinition*>(parent);
  if (classDefinition) result = classDefinition->getProperty(var_name);

  if (result == nullptr) {
    if (scope) {
      result = scope->getVariable(var_name);
    }
  }
  if ((result == nullptr) && scope) {
    Scope* itr_scope = scope;
    while (itr_scope) {
      Procedure* proc = scope_cast<Procedure*>(itr_scope);
      if (proc) {
        for (auto param : proc->getParams()) {
          if (param->getName() == var_name) {
            result = param;
            break;
          }
        }
      }
      if (result) break;
      itr_scope = itr_scope->getParentScope();
    }
  }

  if (result == nullptr && parent) {
    for (auto package : parent->getAccessPackages()) {
      Value* val = package->getValue(var_name);
      if (val) {
        break;
      }
    }
  }

  if ((result == nullptr) && (errtype != ErrorDefinition::NO_ERROR_MESSAGE)) {
    Location loc1(fC->getFileId(id), fC->Line(id), fC->Column(id),
                  symbols->registerSymbol(var_name));
    Location loc2(symbols->registerSymbol(parent->getName()));
    Error err1(errtype, loc1, loc2);
    errors->addError(err1);
  }

  if (!returnClassParam) {
    // Class parameters datatype have no definition and are strings
    if (result) {
      const DataType* dtype = result->getDataType();
      if (dtype && !dtype->getDefinition()) {
        if (dtype->getType() == VObjectType::slStringConst) {
          result = nullptr;
        }
      }
    }
  }

  return result;
}

Variable* ElaborationStep::locateVariable_(std::vector<std::string>& var_chain,
                                           const FileContent* fC, NodeId id,
                                           Scope* scope,
                                           DesignComponent* parentComponent,
                                           ErrorDefinition::ErrorType errtype) {
  Variable* the_obj = nullptr;
  const DesignComponent* currentComponent = parentComponent;
  for (auto var : var_chain) {
    if (var == "this") {
    } else if (var == "super") {
      const ClassDefinition* classDefinition =
          valuedcomponenti_cast<const ClassDefinition*>(currentComponent);
      if (classDefinition) {
        currentComponent = nullptr;
        for (const auto& cc : classDefinition->getBaseClassMap()) {
          currentComponent = datatype_cast<const ClassDefinition*>(cc.second);
          var = "this";
          break;
        }
        if (currentComponent == nullptr) {
          var = "super";
          currentComponent = parentComponent;
        }
      }
    }

    the_obj =
        bindVariable_(var, scope, fC, id, currentComponent, errtype, false);
    if (the_obj) {
      const DataType* dtype = the_obj->getDataType();
      while (dtype && dtype->getDefinition()) {
        dtype = dtype->getDefinition();
      }
      const ClassDefinition* tmpClass =
          datatype_cast<const ClassDefinition*>(dtype);
      if (tmpClass) {
        currentComponent = tmpClass;
      }
    }
  }
  return the_obj;
}

Variable* ElaborationStep::locateStaticVariable_(
    std::vector<std::string>& var_chain, const FileContent* fC, NodeId id,
    Scope* scope, DesignComponent* parentComponent,
    ErrorDefinition::ErrorType errtype) {
  std::string name;
  for (unsigned int i = 0; i < var_chain.size(); i++) {
    name += var_chain[i];
    if (i < var_chain.size() - 1) name += "::";
  }
  std::map<std::string, Variable*>::iterator itr = m_staticVariables.find(name);
  if (itr != m_staticVariables.end()) return (*itr).second;
  Variable* result = nullptr;
  Design* design = m_compileDesign->getCompiler()->getDesign();
  if (!var_chain.empty()) {
    Package* package = design->getPackage(var_chain[0]);
    if (package) {
      if (var_chain.size() > 1) {
        ClassDefinition* classDefinition =
            package->getClassDefinition(var_chain[1]);
        if (classDefinition) {
          if (var_chain.size() == 2) {
            result =
                new Variable(classDefinition, classDefinition->getFileContent(),
                             classDefinition->getNodeId(), InvalidNodeId,
                             classDefinition->getName());
          }
          if (var_chain.size() == 3) {
            std::vector<std::string> tmp;
            tmp.push_back(var_chain[2]);
            result =
                locateVariable_(tmp, fC, id, scope, classDefinition, errtype);
          }
        }
      }
    }

    if (result == nullptr) {
      ClassDefinition* classDefinition =
          design->getClassDefinition(var_chain[0]);
      if (classDefinition == nullptr) {
        std::string name;
        if (parentComponent && parentComponent->getParentScope()) {
          name =
              ((DesignComponent*)parentComponent->getParentScope())->getName();
          name += "::" + var_chain[0];
          classDefinition = design->getClassDefinition(name);
        }
      }
      if (classDefinition) {
        if (var_chain.size() == 1)
          result =
              new Variable(classDefinition, classDefinition->getFileContent(),
                           classDefinition->getNodeId(), InvalidNodeId,
                           classDefinition->getName());
        if (var_chain.size() == 2) {
          std::vector<std::string> tmp;
          tmp.push_back(var_chain[1]);

          const DataType* dtype =
              bindDataType_(var_chain[1], fC, id, classDefinition,
                            ErrorDefinition::NO_ERROR_MESSAGE);
          if (dtype) {
            result =
                new Variable(dtype, dtype->getFileContent(), dtype->getNodeId(),
                             InvalidNodeId, dtype->getName());
          } else
            result =
                locateVariable_(tmp, fC, id, scope, classDefinition, errtype);
        }
      }
    }
  }
  if (result == nullptr) {
    if (!var_chain.empty()) {
      const DataType* dtype =
          bindDataType_(var_chain[0], fC, id, parentComponent, errtype);
      if (dtype) {
        result =
            new Variable(dtype, dtype->getFileContent(), dtype->getNodeId(),
                         InvalidNodeId, dtype->getName());
      }
    }
  }
  m_staticVariables.emplace(name, result);
  return result;
}

void checkIfBuiltInTypeOrErrorOut(DesignComponent* def, const FileContent* fC,
                                  NodeId id, const DataType* type,
                                  const std::string& interfName,
                                  ErrorContainer* errors,
                                  SymbolTable* symbols) {
  if (def == nullptr && type == nullptr && (interfName != "logic") &&
      (interfName != "byte") && (interfName != "bit") &&
      (interfName != "new") && (interfName != "expect") &&
      (interfName != "var") && (interfName != "signed") &&
      (interfName != "unsigned") && (interfName != "do") &&
      (interfName != "final") && (interfName != "global") &&
      (interfName != "soft")) {
    Location loc(fC->getFileId(id), fC->Line(id), fC->Column(id),
                 symbols->registerSymbol(interfName));
    Error err(ErrorDefinition::COMP_UNDEFINED_TYPE, loc);
    errors->addError(err);
  }
}

bool bindStructInPackage(Design* design, Signal* signal,
                         const std::string_view packageName,
                         const std::string_view structName) {
  Package* p = design->getPackage(packageName);
  if (p) {
    const DataType* dtype = p->getDataType(structName);
    if (dtype) {
      signal->setDataType(dtype);
      const DataType* actual = dtype->getActual();
      if (actual->getCategory() == DataType::Category::STRUCT) {
        Struct* st = (Struct*)actual;
        if (st->isNet()) {
          signal->setType(VObjectType::slNetType_Wire);
        }
      }
      return true;
    } else {
      const DataType* dtype = p->getClassDefinition(structName);
      if (dtype) {
        signal->setDataType(dtype);
        return true;
      }
    }
  }
  return false;
}

bool ElaborationStep::bindPortType_(Signal* signal, const FileContent* fC,
                                    NodeId id, Scope* scope,
                                    ModuleInstance* instance,
                                    DesignComponent* parentComponent,
                                    ErrorDefinition::ErrorType errtype) {
  if (signal->getDataType() || signal->getInterfaceDef() ||
      signal->getModPort())
    return true;
  Compiler* compiler = m_compileDesign->getCompiler();
  ErrorContainer* errors = compiler->getErrorContainer();
  SymbolTable* symbols = compiler->getSymbolTable();
  Design* design = compiler->getDesign();
  const std::string& libName = fC->getLibrary()->getName();
  VObjectType type = fC->Type(id);
  switch (type) {
    case VObjectType::slPort:
      /*
        n<mem_if> u<3> t<StringConst> p<6> s<5> l<1>
        n<> u<4> t<Constant_bit_select> p<5> l<1>
        n<> u<5> t<Constant_select> p<6> c<4> l<1>
        n<> u<6> t<Port_reference> p<11> c<3> s<10> l<1>
        n<mif> u<7> t<StringConst> p<10> s<9> l<1>
        n<> u<8> t<Constant_bit_select> p<9> l<1>
        n<> u<9> t<Constant_select> p<10> c<8> l<1>
        n<> u<10> t<Port_reference> p<11> c<7> l<1>
        n<> u<11> t<Port_expression> p<12> c<6> l<1>
        n<> u<12> t<Port> p<13> c<11> l<1>
       */
      {
        NodeId Port_expression = fC->Child(id);
        if (Port_expression &&
            (fC->Type(Port_expression) == VObjectType::slPort_expression)) {
          NodeId if_type = fC->Child(Port_expression);
          if (fC->Type(if_type) == VObjectType::slPort_reference) {
            NodeId if_type_name_s = fC->Child(if_type);
            NodeId if_name = fC->Sibling(if_type);
            if (if_name) {
              std::string interfaceName =
                  libName + "@" + fC->SymName(if_type_name_s);
              ModuleDefinition* interface =
                  design->getModuleDefinition(interfaceName);
              if (interface) {
                signal->setInterfaceDef(interface);
              } else {
                Location loc(fC->getFileId(if_type_name_s),
                             fC->Line(if_type_name_s),
                             fC->Column(if_type_name_s),
                             symbols->registerSymbol(interfaceName));
                Error err(ErrorDefinition::COMP_UNDEFINED_INTERFACE, loc);
                errors->addError(err);
              }
            }
          }
        }
        break;
      }
    case VObjectType::slInput_declaration:
    case VObjectType::slOutput_declaration:
    case VObjectType::slInout_declaration: {
      break;
    }
    case VObjectType::slPort_declaration: {
      /*
       n<Configuration> u<21> t<StringConst> p<22> l<7>
       n<> u<22> t<Interface_identifier> p<26> c<21> s<25> l<7>
       n<cfg> u<23> t<StringConst> p<24> l<7>
       n<> u<24> t<Interface_identifier> p<25> c<23> l<7>
       n<> u<25> t<List_of_interface_identifiers> p<26> c<24> l<7>
       n<> u<26> t<Interface_port_declaration> p<27> c<22> l<7>
       n<> u<27> t<Port_declaration> p<28> c<26> l<7>
       */
      NodeId subNode = fC->Child(id);
      VObjectType subType = fC->Type(subNode);
      switch (subType) {
        case VObjectType::slInterface_port_declaration: {
          NodeId interface_identifier = fC->Child(subNode);
          NodeId interfIdName = fC->Child(interface_identifier);
          const std::string& interfName = fC->SymName(interfIdName);

          DesignComponent* def = nullptr;
          const DataType* type = nullptr;

          const std::pair<FileCNodeId, DesignComponent*>* datatype =
              parentComponent->getNamedObject(interfName);
          if (!datatype) {
            def = design->getClassDefinition(
                StrCat(parentComponent->getName(), "::", interfName));
          }
          if (datatype) {
            def = datatype->second;
          }
          if (def == nullptr) {
            def = design->getComponentDefinition(libName + "@" + interfName);
          }
          if (def == nullptr) {
            type = parentComponent->getDataType(interfName);
          }
          checkIfBuiltInTypeOrErrorOut(def, fC, id, type, interfName, errors,
                                       symbols);
          break;
        }
        case VObjectType::slInput_declaration:
        case VObjectType::slOutput_declaration:
        case VObjectType::slInout_declaration: {
          break;
        }
        default:
          break;
      }
      break;
    }
    case VObjectType::slStringConst: {
      std::string interfName;
      if (signal->getInterfaceTypeNameId()) {
        interfName = signal->getInterfaceTypeName();
      } else {
        if (NodeId typespecId = signal->getTypeSpecId()) {
          if (fC->Type(typespecId) == VObjectType::slClass_scope) {
            NodeId Class_type = fC->Child(typespecId);
            NodeId Class_type_name = fC->Child(Class_type);
            NodeId Class_scope_name = fC->Sibling(typespecId);
            if (bindStructInPackage(design, signal,
                                    fC->SymName(Class_type_name),
                                    fC->SymName(Class_scope_name)))
              return true;
          } else if (fC->Type(typespecId) == VObjectType::slStringConst) {
            interfName = fC->SymName(typespecId);
          }
        }
      }
      std::string baseName = interfName;
      std::string modPort;
      if (interfName.find('.') != std::string::npos) {
        modPort = interfName;
        modPort = StringUtils::ltrim_until(modPort, '.');
        baseName = StringUtils::rtrim_until(baseName, '.');
      } else if (interfName.find("::") != std::string::npos) {
        std::vector<std::string_view> result;
        StringUtils::tokenizeMulti(interfName, "::", result);
        if (result.size() > 1) {
          const std::string_view packName = result[0];
          const std::string_view structName = result[1];
          if (bindStructInPackage(design, signal, packName, structName))
            return true;
        }
      }

      DesignComponent* def = nullptr;
      const DataType* type = nullptr;

      const std::pair<FileCNodeId, DesignComponent*>* datatype =
          parentComponent->getNamedObject(interfName);
      if (datatype) {
        def = datatype->second;
        DataType* dt = valuedcomponenti_cast<ClassDefinition*>(def);
        if (dt) {
          signal->setDataType(dt);
        }
      } else {
        std::string name = StrCat(parentComponent->getName(), "::", interfName);
        def = design->getClassDefinition(name);
        DataType* dt = valuedcomponenti_cast<ClassDefinition*>(def);
        if (dt) {
          signal->setDataType(dt);
        }
      }
      if (def == nullptr) {
        def = design->getComponentDefinition(libName + "@" + baseName);
        if (def) {
          ModuleDefinition* module =
              valuedcomponenti_cast<ModuleDefinition*>(def);
          ClassDefinition* cl = valuedcomponenti_cast<ClassDefinition*>(def);
          if (module) {
            signal->setInterfaceDef(module);
          } else if (cl) {
            signal->setDataType(cl);
            return true;
          } else {
            def = nullptr;
          }
          if (!modPort.empty()) {
            if (module) {
              if (ModPort* modport = module->getModPort(modPort)) {
                signal->setModPort(modport);
              } else {
                def = nullptr;
              }
            }
          }
        }
      }
      if (def == nullptr) {
        def = design->getComponentDefinition(libName + "@" + baseName);
        ClassDefinition* c = valuedcomponenti_cast<ClassDefinition*>(def);
        if (c) {
          Variable* var = new Variable(c, fC, signal->getNodeId(),
                                       InvalidNodeId, signal->getName());
          parentComponent->addVariable(var);
          return false;
        } else {
          def = nullptr;
        }
      }
      if (def == nullptr) {
        type = parentComponent->getDataType(interfName);
        if (type == nullptr) {
          if (!m_compileDesign->getCompiler()
                   ->getCommandLineParser()
                   ->fileunit()) {
            for (const auto& fC : m_compileDesign->getCompiler()
                                      ->getDesign()
                                      ->getAllFileContents()) {
              if (const DataType* dt1 = fC.second->getDataType(interfName)) {
                type = dt1;
                break;
              }
            }
          }
        }

        if (type) {
          const DataType* def = type->getActual();
          DataType::Category cat = def->getCategory();
          if (cat == DataType::Category::SIMPLE_TYPEDEF) {
            VObjectType t = def->getType();
            if (t == VObjectType::slIntVec_TypeLogic) {
              // Make "net types" explicit (vs variable types) for elab.
              signal->setType(VObjectType::slIntVec_TypeLogic);
            } else if (t == VObjectType::slIntVec_TypeReg) {
              signal->setType(VObjectType::slIntVec_TypeReg);
            } else if (t == VObjectType::slNetType_Wire) {
              signal->setType(VObjectType::slNetType_Wire);
            }
          } else if (cat == DataType::Category::REF) {
            // Should not arrive here, there should always be an actual
            // definition
          }
          signal->setDataType(type);
        }
      }
      if (def == nullptr) {
        if (parentComponent->getParameters()) {
          for (auto param : *parentComponent->getParameters()) {
            if (param->UhdmType() == uhdmtype_parameter) {
              if (param->VpiName() == interfName) {
                Parameter* p = parentComponent->getParameter(interfName);
                type = p;
                signal->setDataType(type);
                return true;
              }
            }
          }
        }
      }
      if (signal->getType() != VObjectType::slNoType) {
        return true;
      }
      if (def == nullptr) {
        while (instance) {
          for (Parameter* p : instance->getTypeParams()) {
            if (p->getName() == interfName) {
              type = p;
              signal->setDataType(type);
              return true;
            }
          }

          DesignComponent* component = instance->getDefinition();
          if (component) {
            if (component->getParameters()) {
              for (auto param : *component->getParameters()) {
                if (param->UhdmType() == uhdmtype_parameter) {
                  if (param->VpiName() == interfName) {
                    Parameter* p = component->getParameter(interfName);
                    type = p;
                    signal->setDataType(type);
                    return true;
                  }
                }
              }
            }
          }
          instance = instance->getParent();
        }
      }
      checkIfBuiltInTypeOrErrorOut(def, fC, id, type, interfName, errors,
                                   symbols);
      break;
    }
    default:
      break;
  }
  return true;
}

UHDM::expr* ElaborationStep::exprFromAssign_(DesignComponent* component,
                                             const FileContent* fC, NodeId id,
                                             NodeId unpackedDimension,
                                             ModuleInstance* instance) {
  // Assignment section
  NodeId assignment;
  NodeId Assign = fC->Sibling(id);
  if (Assign && (fC->Type(Assign) == VObjectType::slExpression)) {
    assignment = Assign;
  }
  if (unpackedDimension) {
    NodeId tmp = unpackedDimension;
    while ((fC->Type(tmp) == VObjectType::slUnpacked_dimension) ||
           (fC->Type(tmp) == VObjectType::slVariable_dimension)) {
      tmp = fC->Sibling(tmp);
    }
    if (tmp && (fC->Type(tmp) != VObjectType::slUnpacked_dimension) &&
        (fC->Type(tmp) != VObjectType::slVariable_dimension)) {
      assignment = tmp;
    }
  }

  NodeId expression;
  if (assignment) {
    if (fC->Type(assignment) == VObjectType::slClass_new) {
      expression = assignment;
    } else {
      NodeId Primary = fC->Child(assignment);
      if (fC->Type(assignment) == VObjectType::slExpression) {
        Primary = assignment;
      }
      expression = Primary;
    }
  } else {
    expression = fC->Sibling(id);
    if ((fC->Type(expression) != VObjectType::slExpression) &&
        (fC->Type(expression) != VObjectType::slConstant_expression))
      expression = InvalidNodeId;
  }

  expr* exp = nullptr;
  if (expression) {
    exp = (expr*)m_helper.compileExpression(component, fC, expression,
                                            m_compileDesign, nullptr, instance);
  }
  return exp;
}

UHDM::typespec* ElaborationStep::elabTypeParameter_(DesignComponent* component,
                                                    Parameter* sit,
                                                    ModuleInstance* instance) {
  Serializer& s = m_compileDesign->getSerializer();
  UHDM::any* uparam = sit->getUhdmParam();
  UHDM::typespec* spec = nullptr;
  bool type_param = false;
  if (uparam->UhdmType() == uhdmtype_parameter) {
    spec = (typespec*)((type_parameter*)uparam)->Typespec();
    type_param = true;
  } else {
    spec = (typespec*)((parameter*)uparam)->Typespec();
  }

  const std::string& pname = sit->getName();
  for (Parameter* param : instance->getTypeParams()) {
    // Param override
    if (param->getName() == pname) {
      UHDM::any* uparam = param->getUhdmParam();
      UHDM::typespec* override_spec = nullptr;
      if (uparam == nullptr) {
        if (type_param) {
          type_parameter* tp = s.MakeType_parameter();
          tp->VpiName(pname);
          param->setUhdmParam(tp);
        } else {
          parameter* tp = s.MakeParameter();
          tp->VpiName(pname);
          param->setUhdmParam(tp);
        }
        uparam = param->getUhdmParam();
      }

      if (type_param)
        override_spec = (UHDM::typespec*)((type_parameter*)uparam)->Typespec();
      else
        override_spec = (UHDM::typespec*)((parameter*)uparam)->Typespec();

      if (override_spec == nullptr) {
        ModuleInstance* parent = instance;
        if (ModuleInstance* pinst = instance->getParent()) parent = pinst;
        override_spec = m_helper.compileTypespec(
            component, param->getFileContent(), param->getNodeType(),
            m_compileDesign, nullptr, parent, true);
      }

      if (override_spec) {
        if (type_param)
          ((type_parameter*)uparam)->Typespec(override_spec);
        else
          ((parameter*)uparam)->Typespec(override_spec);
        spec = override_spec;
        spec->VpiParent(uparam);
      }
      break;
    }
  }
  return spec;
}

any* ElaborationStep::makeVar_(DesignComponent* component, Signal* sig,
                               std::vector<UHDM::range*>* packedDimensions,
                               int packedSize,
                               std::vector<UHDM::range*>* unpackedDimensions,
                               int unpackedSize, ModuleInstance* instance,
                               UHDM::VectorOfvariables* vars,
                               UHDM::expr* assignExp, UHDM::typespec* tps) {
  Serializer& s = m_compileDesign->getSerializer();
  const DataType* dtype = sig->getDataType();
  VObjectType subnettype = sig->getType();

  const std::string& signame = sig->getName();
  const FileContent* const fC = sig->getFileContent();

  variables* obj = nullptr;

  if (dtype) {
    dtype = dtype->getActual();
    if (const Enum* en = datatype_cast<const Enum*>(dtype)) {
      enum_var* stv = s.MakeEnum_var();
      stv->Typespec(en->getTypespec());
      obj = stv;
      stv->Expr(assignExp);
    } else if (const Struct* st = datatype_cast<const Struct*>(dtype)) {
      struct_var* stv = s.MakeStruct_var();
      stv->Typespec(st->getTypespec());
      obj = stv;
      stv->Expr(assignExp);
    } else if (const Union* un = datatype_cast<const Union*>(dtype)) {
      union_var* stv = s.MakeUnion_var();
      stv->Typespec(un->getTypespec());
      obj = stv;
      stv->Expr(assignExp);
    } else if (const DummyType* un = datatype_cast<const DummyType*>(dtype)) {
      typespec* tps = un->getTypespec();
      if (tps == nullptr) {
        tps = m_helper.compileTypespec(component, un->getFileContent(),
                                       un->getNodeId(), m_compileDesign,
                                       nullptr, instance, true, true);
        ((DummyType*)un)->setTypespec(tps);
      }
      variables* var = nullptr;
      UHDM_OBJECT_TYPE ttps = tps->UhdmType();
      if (ttps == uhdmenum_typespec) {
        var = s.MakeEnum_var();
      } else if (ttps == uhdmstruct_typespec) {
        var = s.MakeStruct_var();
      } else if (ttps == uhdmunion_typespec) {
        var = s.MakeUnion_var();
      } else if (ttps == uhdmpacked_array_typespec) {
        packed_array_var* avar = s.MakePacked_array_var();
        auto elems = s.MakeAnyVec();
        avar->Elements(elems);
        var = avar;
      } else if (ttps == uhdmarray_typespec) {
        array_var* array_var = s.MakeArray_var();
        array_var->Typespec(s.MakeArray_typespec());
        array_var->VpiArrayType(vpiStaticArray);
        array_var->VpiRandType(vpiNotRand);
        var = array_var;
      } else if (ttps == uhdmint_typespec) {
        var = s.MakeInt_var();
      } else if (ttps == uhdminteger_typespec) {
        var = s.MakeInteger_var();
      } else if (ttps == uhdmbyte_typespec) {
        var = s.MakeByte_var();
      } else if (ttps == uhdmbit_typespec) {
        var = s.MakeBit_var();
      } else if (ttps == uhdmshort_int_typespec) {
        var = s.MakeShort_int_var();
      } else if (ttps == uhdmlong_int_typespec) {
        var = s.MakeLong_int_var();
      } else if (ttps == uhdmstring_typespec) {
        var = s.MakeString_var();
      } else if (ttps == uhdmlogic_typespec) {
        logic_typespec* ltps = (logic_typespec*)tps;
        logic_var* avar = s.MakeLogic_var();
        avar->Ranges(ltps->Ranges());
        var = avar;
      } else {
        var = s.MakeLogic_var();
      }
      var->VpiName(signame);
      var->Typespec(tps);
      var->Expr(assignExp);
      obj = var;
    } else if (const SimpleType* sit =
                   datatype_cast<const SimpleType*>(dtype)) {
      UHDM::typespec* spec = sit->getTypespec();
      spec = m_helper.elabTypespec(component, spec, m_compileDesign, nullptr,
                                   instance);
      variables* var = m_helper.getSimpleVarFromTypespec(spec, packedDimensions,
                                                         m_compileDesign);
      var->Expr(assignExp);
      var->VpiConstantVariable(sig->isConst());
      var->VpiSigned(sig->isSigned());
      var->VpiName(signame);
      var->Typespec(spec);
      obj = var;
    } else if (/*const ClassDefinition* cl = */ datatype_cast<
               const ClassDefinition*>(dtype)) {
      class_var* stv = s.MakeClass_var();
      stv->Typespec(tps);
      obj = stv;
      stv->Expr(assignExp);
    } else if (Parameter* sit = const_cast<Parameter*>(
                   datatype_cast<const Parameter*>(dtype))) {
      UHDM::typespec* spec = elabTypeParameter_(component, sit, instance);
      if (spec) {
        variables* var = m_helper.getSimpleVarFromTypespec(
            spec, packedDimensions, m_compileDesign);
        if (var) {
          var->Expr(assignExp);
          var->VpiConstantVariable(sig->isConst());
          var->VpiSigned(sig->isSigned());
          var->VpiName(signame);
          obj = var;
        }
      }
    }
  } else if (tps) {
    UHDM::UHDM_OBJECT_TYPE tpstype = tps->UhdmType();
    if (tpstype == uhdmstruct_typespec) {
      struct_var* stv = s.MakeStruct_var();
      stv->Typespec(tps);
      stv->VpiName(signame);
      obj = stv;
      stv->Expr(assignExp);
    } else if (tpstype == uhdmlogic_typespec) {
      logic_var* stv = s.MakeLogic_var();
      stv->Typespec(tps);
      stv->VpiName(signame);
      stv->Ranges(packedDimensions);
      obj = stv;
      stv->Expr(assignExp);
    } else if (tpstype == uhdmenum_typespec) {
      enum_var* stv = s.MakeEnum_var();
      stv->Typespec(tps);
      stv->VpiName(signame);
      obj = stv;
      stv->Expr(assignExp);
    } else if (tpstype == uhdmbit_typespec) {
      bit_var* stv = s.MakeBit_var();
      stv->Typespec(tps);
      stv->VpiName(signame);
      stv->Ranges(unpackedDimensions);
      obj = stv;
      stv->Expr(assignExp);
    } else if (tpstype == uhdmbyte_typespec) {
      byte_var* stv = s.MakeByte_var();
      stv->Typespec(tps);
      stv->VpiName(signame);
      obj = stv;
      stv->Expr(assignExp);
    } else if (tpstype == uhdmreal_typespec) {
      real_var* stv = s.MakeReal_var();
      stv->Typespec(tps);
      stv->VpiName(signame);
      obj = stv;
      stv->Expr(assignExp);
    } else if (tpstype == uhdmint_typespec) {
      int_var* stv = s.MakeInt_var();
      stv->Typespec(tps);
      stv->VpiName(signame);
      obj = stv;
      stv->Expr(assignExp);
    } else if (tpstype == uhdminteger_typespec) {
      integer_var* stv = s.MakeInteger_var();
      stv->Typespec(tps);
      stv->VpiName(signame);
      obj = stv;
      stv->Expr(assignExp);
    } else if (tpstype == uhdmlong_int_typespec) {
      long_int_var* stv = s.MakeLong_int_var();
      stv->Typespec(tps);
      stv->VpiName(signame);
      obj = stv;
      stv->Expr(assignExp);
    } else if (tpstype == uhdmshort_int_typespec) {
      short_int_var* stv = s.MakeShort_int_var();
      stv->Typespec(tps);
      stv->VpiName(signame);
      obj = stv;
      stv->Expr(assignExp);
    } else if (tpstype == uhdmstring_typespec) {
      string_var* stv = s.MakeString_var();
      stv->Typespec(tps);
      stv->VpiName(signame);
      obj = stv;
      stv->Expr(assignExp);
    } else if (tpstype == uhdmbit_typespec) {
      bit_var* stv = s.MakeBit_var();
      stv->Typespec(tps);
      stv->VpiName(signame);
      obj = stv;
      stv->Expr(assignExp);
    } else if (tpstype == uhdmbyte_typespec) {
      byte_var* stv = s.MakeByte_var();
      stv->Typespec(tps);
      stv->VpiName(signame);
      obj = stv;
      stv->Expr(assignExp);
    } else if (tpstype == uhdmtime_typespec) {
      time_var* stv = s.MakeTime_var();
      stv->Typespec(tps);
      stv->VpiName(signame);
      obj = stv;
      stv->Expr(assignExp);
    } else if (tpstype == uhdmunion_typespec) {
      union_var* stv = s.MakeUnion_var();
      stv->Typespec(tps);
      stv->VpiName(signame);
      obj = stv;
      stv->Expr(assignExp);
    } else if (tpstype == uhdmclass_typespec) {
      class_var* stv = s.MakeClass_var();
      stv->Typespec(tps);
      stv->VpiName(signame);
      tps->VpiParent(stv);
      obj = stv;
      stv->Expr(assignExp);
    }
  }

  if (obj == nullptr) {
    variables* var = nullptr;
    if (subnettype == VObjectType::slIntegerAtomType_Shortint) {
      UHDM::short_int_var* int_var = s.MakeShort_int_var();
      var = int_var;
      tps = s.MakeShort_int_typespec();
      int_var->Typespec(tps);
    } else if (subnettype == VObjectType::slIntegerAtomType_Int) {
      UHDM::int_var* int_var = s.MakeInt_var();
      var = int_var;
      tps = s.MakeInt_typespec();
      int_var->Typespec(tps);
    } else if (subnettype == VObjectType::slIntegerAtomType_Integer) {
      UHDM::integer_var* int_var = s.MakeInteger_var();
      var = int_var;
      tps = s.MakeInteger_typespec();
      int_var->Typespec(tps);
    } else if (subnettype == VObjectType::slIntegerAtomType_LongInt) {
      UHDM::long_int_var* int_var = s.MakeLong_int_var();
      var = int_var;
      tps = s.MakeLong_int_typespec();
      int_var->Typespec(tps);
    } else if (subnettype == VObjectType::slIntegerAtomType_Time) {
      UHDM::time_var* int_var = s.MakeTime_var();
      var = int_var;
    } else if (subnettype == VObjectType::slIntVec_TypeBit) {
      UHDM::bit_var* int_var = s.MakeBit_var();
      bit_typespec* btps = s.MakeBit_typespec();
      btps->Ranges(packedDimensions);
      tps = btps;
      int_var->Typespec(tps);
      int_var->Ranges(packedDimensions);
      var = int_var;
    } else if (subnettype == VObjectType::slIntegerAtomType_Byte) {
      UHDM::byte_var* int_var = s.MakeByte_var();
      byte_typespec* btps = s.MakeByte_typespec();
      tps = btps;
      int_var->Typespec(tps);
      var = int_var;
    } else if (subnettype == VObjectType::slNonIntType_ShortReal) {
      UHDM::short_real_var* int_var = s.MakeShort_real_var();
      var = int_var;
    } else if (subnettype == VObjectType::slNonIntType_Real) {
      UHDM::real_var* int_var = s.MakeReal_var();
      var = int_var;
    } else if (subnettype == VObjectType::slNonIntType_RealTime) {
      UHDM::time_var* int_var = s.MakeTime_var();
      var = int_var;
    } else if (subnettype == VObjectType::slString_type) {
      UHDM::string_var* int_var = s.MakeString_var();
      var = int_var;
    } else if (subnettype == VObjectType::slChandle_type) {
      UHDM::chandle_var* chandle_var = s.MakeChandle_var();
      var = chandle_var;
    } else if (subnettype == VObjectType::slIntVec_TypeLogic) {
      logic_var* logicv = s.MakeLogic_var();
      logicv->Ranges(packedDimensions);
      logic_typespec* ltps = s.MakeLogic_typespec();
      ltps->Ranges(packedDimensions);
      NodeId id;
      if (sig->getPackedDimension()) id = fC->Parent(sig->getPackedDimension());
      if (!id) id = sig->getNodeId();
      if (id) {
        fC->populateCoreMembers(id, id, ltps);
      }
      tps = ltps;
      logicv->Typespec(tps);
      var = logicv;
    } else if (subnettype == VObjectType::slEvent_type) {
      named_event* event = s.MakeNamed_event();
      event->VpiName(signame);
      if (instance) {
        Netlist* netlist = instance->getNetlist();
        VectorOfnamed_event* events = netlist->named_events();
        if (events == nullptr) {
          netlist->named_events(s.MakeNamed_eventVec());
          events = netlist->named_events();
        }
        events->push_back(event);
      }
      return event;
    } else {
      // default type (fallback)
      logic_var* logicv = s.MakeLogic_var();
      logicv->Ranges(packedDimensions);
      var = logicv;
    }
    var->VpiSigned(sig->isSigned());
    var->VpiConstantVariable(sig->isConst());
    var->VpiName(signame);
    var->Expr(assignExp);
    obj = var;
  } else if (packedDimensions && (obj->UhdmType() != uhdmlogic_var) &&
             (obj->UhdmType() != uhdmbit_var) &&
             (obj->UhdmType() != uhdmpacked_array_var)) {
    // packed struct array ...
    UHDM::packed_array_var* parray = s.MakePacked_array_var();
    parray->Ranges(packedDimensions);
    VectorOfany* elements = s.MakeAnyVec();
    elements->push_back(obj);
    parray->Elements(elements);
    obj->VpiParent(parray);
    parray->VpiName(signame);
    obj = parray;
  }

  if (unpackedDimensions) {
    array_var* array_var = s.MakeArray_var();
    array_var->Variables(s.MakeVariablesVec());
    bool dynamic = false;
    bool associative = false;
    bool queue = false;
    int index = 0;
    for (auto itr = unpackedDimensions->begin();
         itr != unpackedDimensions->end(); itr++) {
      range* r = *itr;
      const expr* rhs = r->Right_expr();
      if (rhs->UhdmType() == uhdmconstant) {
        const std::string& value = rhs->VpiValue();
        if (value == "STRING:$") {
          queue = true;
          unpackedDimensions->erase(itr);
          break;
        } else if (value == "STRING:associative") {
          associative = true;
          const typespec* tp = rhs->Typespec();
          array_typespec* taps = s.MakeArray_typespec();
          taps->Index_typespec((typespec*)tp);
          array_var->Typespec(taps);
          unpackedDimensions->erase(itr);
          break;
        } else if (value == "STRING:unsized") {
          dynamic = true;
          unpackedDimensions->erase(itr);
          break;
        }
      }
      index++;
    }

    if (associative || queue || dynamic) {
      if (!unpackedDimensions->empty()) {
        if (index == 0) {
          array_var->Ranges(unpackedDimensions);
        } else {
          array_typespec* tps = s.MakeArray_typespec();
          array_var->Typespec(tps);

          if (associative)
            tps->VpiArrayType(vpiAssocArray);
          else if (queue)
            tps->VpiArrayType(vpiQueueArray);
          else if (dynamic)
            tps->VpiArrayType(vpiDynamicArray);
          else
            tps->VpiArrayType(vpiStaticArray);
          array_typespec* subtps = s.MakeArray_typespec();
          tps->Elem_typespec(subtps);

          subtps->Ranges(unpackedDimensions);
          switch (obj->UhdmType()) {
            case uhdmint_var: {
              int_typespec* ts = s.MakeInt_typespec();
              subtps->Elem_typespec(ts);
              break;
            }
            case uhdminteger_var: {
              integer_typespec* ts = s.MakeInteger_typespec();
              subtps->Elem_typespec(ts);
              break;
            }
            case uhdmlogic_var: {
              logic_typespec* ts = s.MakeLogic_typespec();
              subtps->Elem_typespec(ts);
              break;
            }
            case uhdmlong_int_var: {
              long_int_typespec* ts = s.MakeLong_int_typespec();
              subtps->Elem_typespec(ts);
              break;
            }
            case uhdmshort_int_var: {
              short_int_typespec* ts = s.MakeShort_int_typespec();
              subtps->Elem_typespec(ts);
              break;
            }
            case uhdmbyte_var: {
              byte_typespec* ts = s.MakeByte_typespec();
              subtps->Elem_typespec(ts);
              break;
            }
            case uhdmbit_var: {
              bit_typespec* ts = s.MakeBit_typespec();
              subtps->Elem_typespec(ts);
              break;
            }
            case uhdmstring_var: {
              string_typespec* ts = s.MakeString_typespec();
              subtps->Elem_typespec(ts);
              break;
            }
            default: {
              unsupported_typespec* ts = s.MakeUnsupported_typespec();
              subtps->Elem_typespec(ts);
              break;
            }
          }
        }
      }
    }

    if (associative) {
      array_var->VpiArrayType(vpiAssocArray);
    } else if (queue) {
      array_var->VpiArrayType(vpiQueueArray);
    } else if (dynamic) {
      array_var->VpiArrayType(vpiDynamicArray);
    } else {
      array_var->Ranges(unpackedDimensions);
      array_var->VpiArrayType(vpiStaticArray);
    }
    array_var->VpiSize(unpackedSize);
    array_var->VpiName(signame);
    array_var->VpiRandType(vpiNotRand);
    array_var->VpiVisibility(vpiPublicVis);
    vars->push_back(array_var);
    obj->VpiParent(array_var);
    if ((array_var->Typespec() == nullptr) || associative) {
      VectorOfvariables* array_vars = array_var->Variables();
      array_vars->push_back((variables*)obj);
      ((variables*)obj)->VpiName("");
    }
    if (array_var->Typespec() == nullptr) {
      array_var->Typespec(s.MakeArray_typespec());
    }
    array_var->Expr(assignExp);
    fC->populateCoreMembers(sig->getNodeId(), sig->getNodeId(), obj);
    obj = array_var;
  } else {
    if (obj->UhdmType() == uhdmenum_var) {
      ((enum_var*)obj)->VpiName(signame);
    } else if (obj->UhdmType() == uhdmstruct_var) {
      ((struct_var*)obj)->VpiName(signame);
    } else if (obj->UhdmType() == uhdmunion_var) {
      ((union_var*)obj)->VpiName(signame);
    } else if (obj->UhdmType() == uhdmclass_var) {
      ((class_var*)obj)->VpiName(signame);
    } else if (obj->UhdmType() == uhdmlogic_var) {
      ((logic_var*)obj)->VpiName(signame);
    }
    vars->push_back((variables*)obj);
  }

  if (assignExp) {
    if (assignExp->UhdmType() == uhdmconstant) {
      m_helper.adjustSize(tps, component, m_compileDesign, instance,
                          (constant*)assignExp);
    } else if (assignExp->UhdmType() == uhdmoperation) {
      operation* op = (operation*)assignExp;
      int opType = op->VpiOpType();
      const typespec* tp = tps;
      if (opType == vpiAssignmentPatternOp) {
        if (tp->UhdmType() == uhdmpacked_array_typespec) {
          packed_array_typespec* ptp = (packed_array_typespec*)tp;
          tp = dynamic_cast<const typespec*>(ptp->Elem_typespec());
          if (tp == nullptr) {
            tp = tps;
          }
        }
      }
      for (auto oper : *op->Operands()) {
        if (oper->UhdmType() == uhdmconstant)
          m_helper.adjustSize(tp, component, m_compileDesign, instance,
                              (constant*)oper);
      }
    }
  }

  if (obj) {
    UHDM::ExprEval eval;
    obj->Expr(eval.flattenPatternAssignments(s, tps, assignExp));
    obj->VpiSigned(sig->isSigned());
    obj->VpiConstantVariable(sig->isConst());
    obj->VpiIsRandomized(sig->isRand() || sig->isRandc());
    if (sig->isRand())
      obj->VpiRandType(vpiRand);
    else if (sig->isRandc())
      obj->VpiRandType(vpiRandC);
    if (sig->isStatic()) {
      obj->VpiAutomatic(false);
    } else {
      obj->VpiAutomatic(true);
    }
    if (sig->isProtected()) {
      obj->VpiVisibility(vpiProtectedVis);
    } else if (sig->isLocal()) {
      obj->VpiVisibility(vpiLocalVis);
    } else {
      obj->VpiVisibility(vpiPublicVis);
    }
  }
  return obj;
}
}  // namespace SURELOG

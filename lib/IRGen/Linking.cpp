//===--- Linking.cpp - Name mangling for IRGen entities -------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
//  This file implements name mangling for IRGen entities with linkage.
//
//===----------------------------------------------------------------------===//

#include "Linking.h"
#include "llvm/Support/raw_ostream.h"
#include "swift/Basic/Fallthrough.h"
#include "swift/SIL/SILGlobalVariable.h"
#include "swift/AST/Mangle.h"
#include "clang/AST/Attr.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclObjC.h"

using namespace swift;
using namespace irgen;
using namespace Mangle;

static bool isDeallocating(DestructorKind kind) {
  switch (kind) {
  case DestructorKind::Deallocating: return true;
  case DestructorKind::Destroying: return false;
  }
  llvm_unreachable("bad destructor kind");
}

static bool isAllocating(ConstructorKind kind) {
  switch (kind) {
  case ConstructorKind::Allocating: return true;
  case ConstructorKind::Initializing: return false;
  }
  llvm_unreachable("bad constructor kind");
}

static StringRef mangleValueWitness(ValueWitness witness) {
  // The ones with at least one capital are the composite ops, and the
  // capitals correspond roughly to the positions of buffers (as
  // opposed to objects) in the arguments.  That doesn't serve any
  // direct purpose, but it's neat.
  switch (witness) {
  case ValueWitness::AllocateBuffer: return "al";
  case ValueWitness::AssignWithCopy: return "ca";
  case ValueWitness::AssignWithTake: return "ta";
  case ValueWitness::DeallocateBuffer: return "de";
  case ValueWitness::Destroy: return "xx";
  case ValueWitness::DestroyBuffer: return "XX";
  case ValueWitness::InitializeBufferWithCopyOfBuffer: return "CP";
  case ValueWitness::InitializeBufferWithCopy: return "Cp";
  case ValueWitness::InitializeWithCopy: return "cp";
  case ValueWitness::InitializeBufferWithTake: return "Tk";
  case ValueWitness::InitializeWithTake: return "tk";
  case ValueWitness::ProjectBuffer: return "pr";
  case ValueWitness::TypeOf: return "ty";
  case ValueWitness::StoreExtraInhabitant: return "xs";
  case ValueWitness::GetExtraInhabitantIndex: return "xg";
  case ValueWitness::GetEnumTag: return "ug";
  case ValueWitness::InplaceProjectEnumData: return "up";
      
  case ValueWitness::Size:
  case ValueWitness::Flags:
  case ValueWitness::Stride:
  case ValueWitness::ExtraInhabitantFlags:
    llvm_unreachable("not a function witness");
  }
  llvm_unreachable("bad witness kind");
}

/// Mangle this entity into the given buffer.
void LinkEntity::mangle(SmallVectorImpl<char> &buffer) const {
  llvm::raw_svector_ostream stream(buffer);
  mangle(stream);
}

/// Mangle this entity into the given stream.
void LinkEntity::mangle(raw_ostream &buffer) const {
  // Almost everything below gets the common prefix:
  //   mangled-name ::= '_T' global
  Mangler mangler(buffer);
  switch (getKind()) {
  // FIXME: Mangle a more descriptive symbol name for anonymous funcs.
  case Kind::AnonymousFunction:
    buffer << "closure";
    return;
      
  //   global ::= 'w' value-witness-kind type     // value witness
  case Kind::ValueWitness:
    buffer << "_Tw";
    buffer << mangleValueWitness(getValueWitness());
   
    mangler.mangleType(getType(), ExplosionKind::Minimal, 0);
    return;

  //   global ::= 'WV' type                       // value witness
  case Kind::ValueWitnessTable:
    buffer << "_TWV";
    mangler.mangleType(getType(), ExplosionKind::Minimal, 0);
    return;

  //   global ::= 't' type
  // Abstract type manglings just follow <type>.
  case Kind::TypeMangling:
    mangler.mangleType(getType(), ExplosionKind::Minimal, 0);
    return;

  //   global ::= 'M' directness type             // type metadata
  //   global ::= 'MP' directness type            // type metadata pattern
  case Kind::TypeMetadata: {
    buffer << "_TM";
    bool isPattern = isMetadataPattern();
    if (isPattern) buffer << 'P';
    mangler.mangleDirectness(isMetadataIndirect());
    mangler.mangleType(getType(), ExplosionKind::Minimal, 0);
    return;
  }

  //   global ::= 'Mm' type                       // class metaclass
  case Kind::SwiftMetaclassStub:
    buffer << "_TMm";
    mangler.mangleNominalType(cast<ClassDecl>(getDecl()),
                              ExplosionKind::Minimal,
                              Mangler::BindGenerics::None);
    return;
      
  //   global ::= 'Mn' type                       // nominal type descriptor
  case Kind::NominalTypeDescriptor:
    buffer << "_TMn";
    mangler.mangleNominalType(cast<NominalTypeDecl>(getDecl()),
                              ExplosionKind::Minimal,
                              Mangler::BindGenerics::None);
    return;

  //   global ::= 'Mp' type                       // protocol descriptor
  case Kind::ProtocolDescriptor:
    buffer << "_TMp";
    mangler.mangleProtocolName(cast<ProtocolDecl>(getDecl()));
    return;
      
  //   global ::= 'Wo' entity
  case Kind::WitnessTableOffset:
    buffer << "_TWo";
    mangler.mangleEntity(getDecl(), getExplosionKind(), getUncurryLevel());
    return;

  //   global ::= 'Wv' directness entity
  case Kind::FieldOffset:
    buffer << "_TWv";
    mangler.mangleDirectness(isOffsetIndirect());
    mangler.mangleEntity(getDecl(), ExplosionKind::Minimal, 0);
    return;
      
  //   global ::= 'WP' protocol-conformance
  case Kind::DirectProtocolWitnessTable:
    buffer << "_TWP";
    mangler.mangleProtocolConformance(getProtocolConformance());
    return;

  //   global ::= 'WZ' protocol-conformance
  case Kind::LazyProtocolWitnessTableAccessor:
    buffer << "_TWZ";
    mangler.mangleProtocolConformance(getProtocolConformance());
    return;
      
  //   global ::= 'Wz' protocol-conformance
  case Kind::LazyProtocolWitnessTableTemplate:
    buffer << "_TWz";
    mangler.mangleProtocolConformance(getProtocolConformance());
    return;
  
  //   global ::= 'WD' protocol-conformance
  case Kind::DependentProtocolWitnessTableGenerator:
    buffer << "_TWD";
    mangler.mangleProtocolConformance(getProtocolConformance());
    return;
      
  //   global ::= 'Wd' protocol-conformance
  case Kind::DependentProtocolWitnessTableTemplate:
    buffer << "_TWd";
    mangler.mangleProtocolConformance(getProtocolConformance());
    return;

  //   global ::= 'Tb' type
  case Kind::BridgeToBlockConverter:
    buffer << "_TTb";
    mangler.mangleType(getType(), ExplosionKind::Minimal, 0);
    return;

  // For all the following, this rule was imposed above:
  //   global ::= local-marker? entity            // some identifiable thing

  //   entity ::= context 'D'                     // deallocating destructor
  //   entity ::= context 'd'                     // non-deallocating destructor
  case Kind::Destructor:
    buffer << "_T";
    mangler.mangleDestructorEntity(cast<DestructorDecl>(getDecl()),
                                   isDeallocating(getDestructorKind()));
    return;

  //   entity ::= context 'C' type                // allocating constructor
  //   entity ::= context 'c' type                // non-allocating constructor
  case Kind::Constructor: {
    buffer << "_T";
    mangler.mangleConstructorEntity(cast<ConstructorDecl>(getDecl()),
                                    isAllocating(getConstructorKind()),
                                    getExplosionKind(), getUncurryLevel());
    return;
  }

  //   entity ::= declaration                     // other declaration
  case Kind::Function:
    // As a special case, functions can have external asm names.
    if (!getDecl()->getAttrs().AsmName.empty()) {
      buffer << getDecl()->getAttrs().AsmName;
      return;
    }

    // Otherwise, fall through into the 'other decl' case.
    SWIFT_FALLTHROUGH;

  case Kind::Other:
    // As a special case, Clang functions and globals don't get mangled at all.
    // FIXME: When we can import C++, use Clang's mangler.
    if (auto clangDecl = getDecl()->getClangDecl()) {
      if (auto namedClangDecl = dyn_cast<clang::DeclaratorDecl>(clangDecl)) {
        if (auto asmLabel = namedClangDecl->getAttr<clang::AsmLabelAttr>()) {
          buffer << '\01' << asmLabel->getLabel();
        } else {
          buffer << namedClangDecl->getName();
        }
        return;
      }
    }

    buffer << "_T";
    if (auto type = dyn_cast<NominalTypeDecl>(getDecl())) {
      mangler.mangleNominalType(type, getExplosionKind(),
                                Mangler::BindGenerics::None);
    } else {
      mangler.mangleEntity(getDecl(), getExplosionKind(), getUncurryLevel());
    }
    return;

  //   entity ::= declaration 'g'                 // getter
  case Kind::Getter:
    buffer << "_T";
    mangler.mangleGetterEntity(getDecl(), getExplosionKind());
    return;

  //   entity ::= declaration 's'                 // setter
  case Kind::Setter:
    buffer << "_T";
    mangler.mangleSetterEntity(getDecl(), getExplosionKind());
    return;

  // An Objective-C class reference;  not a swift mangling.
  case Kind::ObjCClass:
    buffer << "OBJC_CLASS_$_" << getDecl()->getName().str();
    return;

  // An Objective-C metaclass reference;  not a swift mangling.
  case Kind::ObjCMetaclass:
    buffer << "OBJC_METACLASS_$_" << getDecl()->getName().str();
    return;
  case Kind::SILFunction:
    buffer << getSILFunction()->getName();
    return;
  case Kind::SILGlobalVariable:
    buffer << getSILGlobalVariable()->getName();
    return;
  }
  llvm_unreachable("bad entity kind!");
}

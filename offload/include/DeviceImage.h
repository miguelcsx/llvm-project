//===-- DeviceImage.h - Representation of the device code/image -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//
//===----------------------------------------------------------------------===//

#ifndef OMPTARGET_DEVICE_IMAGE_H
#define OMPTARGET_DEVICE_IMAGE_H

#include "OffloadEntry.h"
#include "Shared/APITypes.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/iterator.h"
#include "llvm/ADT/iterator_range.h"
#include "llvm/Object/OffloadBinary.h"

#include <memory>

class DeviceImageTy {

  std::unique_ptr<llvm::object::OffloadBinary> Binary;

  __tgt_bin_desc *BinaryDesc;
  __tgt_device_image Image;

public:
  DeviceImageTy(__tgt_bin_desc &BinaryDesc, __tgt_device_image &Image);

  __tgt_device_image &getExecutableImage() { return Image; }
  const __tgt_device_image &getExecutableImage() const { return Image; }
  __tgt_bin_desc &getBinaryDesc() { return *BinaryDesc; }
  const __tgt_bin_desc &getBinaryDesc() const { return *BinaryDesc; }

  bool hasOffloadBinary() const { return Binary != nullptr; }

  const llvm::object::OffloadBinary *getOffloadBinary() const {
    return Binary.get();
  }

  llvm::object::ImageKind getImageKind() const {
    return Binary ? Binary->getImageKind() : llvm::object::IMG_None;
  }

  llvm::object::OffloadKind getOffloadKind() const {
    return Binary ? Binary->getOffloadKind() : llvm::object::OFK_None;
  }

  llvm::StringRef getTriple() const {
    return Binary ? Binary->getTriple() : llvm::StringRef();
  }

  llvm::StringRef getArch() const {
    return Binary ? Binary->getArch() : llvm::StringRef();
  }

  llvm::StringRef getString(llvm::StringRef Key) const {
    return Binary ? Binary->getString(Key) : llvm::StringRef();
  }

  llvm::StringRef getExecutableBinary() const {
    if (!Image.ImageStart || !Image.ImageEnd)
      return llvm::StringRef();
    auto *Begin = reinterpret_cast<const char *>(Image.ImageStart);
    auto *End = reinterpret_cast<const char *>(Image.ImageEnd);
    return llvm::StringRef(Begin, End - Begin);
  }

  auto entries() {
    return llvm::make_range(Image.EntriesBegin, Image.EntriesEnd);
  }
};

#endif // OMPTARGET_DEVICE_IMAGE_H

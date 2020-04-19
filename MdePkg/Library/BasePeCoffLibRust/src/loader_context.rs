// @file -- loader_context.rs
//
// Copyright (c) Microsoft Corporation.
// SPDX-License-Identifier: BSD-2-Clause-Patent
//

#![cfg_attr(not(test), no_std)]
#![allow(unused)]

// Enable printing in both test and build.
// Ewwww...
#[cfg(test)]
extern crate std;
#[cfg(test)]
use std::{println, eprintln};
#[cfg(not(test))]
use uefi_rust_print_lib_debug_lib::{println, eprintln};

use alloc::vec::Vec;
use alloc::boxed::Box;
use r_efi::{efi, base};


// typedef
// RETURN_STATUS
// (EFIAPI *PE_COFF_LOADER_READ_FILE)(
//   IN     VOID   *FileHandle,
//   IN     UINTN  FileOffset,
//   IN OUT UINTN  *ReadSize,
//   OUT    VOID   *Buffer
//   );
type PeCoffLoaderReadFile = extern "win64" fn(_: *const core::ffi::c_void,
                                              _: usize,
                                              _: *mut usize,
                                              _: *mut core::ffi::c_void) -> efi::Status;

#[repr(u32)]
#[derive(Clone,Copy,Debug,PartialEq)]
pub enum PeCoffImageError {
  ImageErrorSuccess = 0,
  ImageErrorImageRead,
  ImageErrorInvalidPeHeaderSignature,
  ImageErrorInvalidMachineType,
  ImageErrorInvalidSubsystem,
  ImageErrorInvalidImageAddress,
  ImageErrorInvalidImageSize,
  ImageErrorInvalidSectionAlignment,
  ImageErrorSectionNotLoaded,
  ImageErrorFailedRelocation,
  ImageErrorFailedIcacheFlush,
  ImageErrorUnsupported,
}

impl From<goblin::error::Error> for PeCoffImageError {
    fn from(err: goblin::error::Error) -> Self {
      // Let's just say -- for now -- than any Goblin error is an image error.
      PeCoffImageError::ImageErrorImageRead
    }
}

// REF: MdePkg/Include/Library/PeCoffLib.h
#[repr(C)]
pub struct PeCoffLoaderImageContext {
  image_address:          base::PhysicalAddress,
  image_size:             u64,
  destination_address:    base::PhysicalAddress,
  entry_point:            base::PhysicalAddress,
  image_read:             PeCoffLoaderReadFile,
  handle:                 *const core::ffi::c_void,
  fixup_data:             *const core::ffi::c_void,
  section_alignment:      u32,
  pe_coff_header_offset:  u32,
  debug_directory_entry_rva:  u32,
  code_view:              *const core::ffi::c_void,
  pdb_pointer:            *const u8,
  size_of_headers:        usize,
  image_code_memory_type: u32,
  image_data_memory_type: u32,
  image_error:            PeCoffImageError,
  fixup_data_size:        usize,
  machine:                u16,
  image_type:             u16,
  relocations_stripped:   base::Boolean,
  is_te_image:            base::Boolean,
  hii_resource_data:      base::PhysicalAddress,
  context:                u64
}

// Have to define this manually because of the extern functions.
impl core::fmt::Debug for PeCoffLoaderImageContext {
  fn fmt(&self, f: &mut core::fmt::Formatter) -> core::fmt::Result {
    f.debug_struct("PeCoffLoaderImageContext")
      .field("image_address", &self.image_address)
      .field("image_size", &self.image_size)
      .field("destination_address", &self.destination_address)
      .field("entry_point", &self.entry_point)
      .field("image_read", &(self.image_read as *const ()))
      .field("handle", &self.handle)
      .field("fixup_data", &self.fixup_data)
      .field("section_alignment", &self.section_alignment)
      .field("pe_coff_header_offset", &self.pe_coff_header_offset)
      .field("debug_directory_entry_rva", &self.debug_directory_entry_rva)
      .field("code_view", &self.code_view)
      .field("pdb_pointer", &self.pdb_pointer)
      .field("size_of_headers", &self.size_of_headers)
      .field("image_code_memory_type", &self.image_code_memory_type)
      .field("image_data_memory_type", &self.image_data_memory_type)
      .field("image_error", &self.image_error)
      .field("fixup_data_size", &self.fixup_data_size)
      .field("machine", &self.machine)
      .field("image_type", &self.image_type)
      .field("relocations_stripped", &self.relocations_stripped)
      .field("is_te_image", &self.is_te_image)
      .field("hii_resource_data", &self.hii_resource_data)
      .field("context", &self.context)
      .finish()
  }
}

impl PeCoffLoaderImageContext {
  fn new(image_read_fn: PeCoffLoaderReadFile) -> Self {
    Self {
      image_address:          0,
      image_size:             0,
      destination_address:    0,
      entry_point:            0,
      image_read:             image_read_fn,
      handle:                 core::ptr::null(),
      fixup_data:             core::ptr::null(),
      section_alignment:      0,
      pe_coff_header_offset:  0,
      debug_directory_entry_rva:  0,
      code_view:              core::ptr::null(),
      pdb_pointer:            core::ptr::null(),
      size_of_headers:        0,
      image_code_memory_type: 0,
      image_data_memory_type: 0,
      image_error:            PeCoffImageError::ImageErrorSuccess,
      fixup_data_size:        0,
      machine:                0,
      image_type:             0,
      relocations_stripped:   base::Boolean::FALSE,
      is_te_image:            base::Boolean::FALSE,
      hii_resource_data:      0,
      context:                0
    }
  }

  fn is_raw_struture_valid(&self) -> bool {
    // Validate the loaded raw data against fundemental expectations
    // of all callers and consumers.

    // Make sure that the image_read function is populated.
    if (self.image_read as *const ()).is_null() {
      return false;
    }

    true
  }

  pub unsafe fn from_raw(ptr: *mut Self) -> Result<&'static mut Self, ()> {
    if ptr.is_null() {
      return Err(())
    }

    let image_context = core::mem::transmute::<*mut Self, &'static mut Self>(ptr);

    // image_error is never used for communicating TO this structure, so
    // we can safely initialize it to a known value, rather than bounds-check it.
    image_context.image_error = PeCoffImageError::ImageErrorSuccess;

    if image_context.is_raw_struture_valid() {
      Ok(image_context)
    }
    else {
      Err(())
    }
  }

  fn test_offset(&self, offset: usize) -> bool {
    let mut test_slice: [u8; 1] = [0];
    let result = self.read_image_into(offset, &mut test_slice);
    result.is_ok() && result.unwrap() == test_slice.len()
  }

  fn read_image_into(&self, offset: usize, buffer: &mut [u8]) -> Result<usize, PeCoffImageError> {
    let mut read_size = buffer.len();
    let result = unsafe {
      (self.image_read)(self.handle,
                        offset,
                        &mut read_size,
                        buffer.as_mut_ptr() as *mut core::ffi::c_void)
    };
    match result {
      efi::Status::SUCCESS => {
        if read_size <= buffer.len() {
          Ok(read_size)
        }
        else {
          Err(PeCoffImageError::ImageErrorImageRead)
        }
      },
      _ => Err(PeCoffImageError::ImageErrorImageRead)
    }
  }

  fn read_image(&self, offset: usize, size: usize) -> Result<Vec<u8>, PeCoffImageError> {
    let mut buffer = Vec::with_capacity(size) as Vec<u8>;
    unsafe { buffer.set_len(size) };
    let read_size = self.read_image_into(offset, &mut buffer)?;

    // If we were successful, we *must* set the length before returning.
    // According to the contract of the function, "size" will have been updated
    // with the bytes actually written.
    unsafe { buffer.set_len(read_size) };
    Ok(buffer)
  }

  pub fn update_info_from_headers(&mut self) -> Result<(), PeCoffImageError> {
    // let dos_header_buffer = self.read_image(0, Self::DOS_HEADER_SIZE)?;
    // let dos_header = goblin::pe::header::DosHeader::parse(&dos_header_buffer)?;
    // if dos_header.signature != goblin::pe::header::DOS_MAGIC {
    //   return Err(PeCoffImageError::ImageErrorImageRead);
    // }
    // self.pe_coff_header_offset = dos_header.pe_pointer;

    // let optional_header_buffer = self.read_image(self.pe_coff_header_offset as usize, Self::OPTIONAL_HEADER_UNION_SIZE)?;

    // SURE,
    // That's one way to do it, and maybe the most efficient way.
    // But we're here to do things easily, not efficiently.
    let file_data = self.read_image(0, self.image_size as usize)?;
    let pe_metadata = goblin::pe::PE::parse(&file_data)?;

    println!("{:?}", pe_metadata);

    self.image_error = PeCoffImageError::ImageErrorUnsupported;
    Err(PeCoffImageError::ImageErrorUnsupported)
  }
}

#[cfg(test)]
mod ffi_context_tests {
  use super::*;
  use alloc::vec;
  use core::{mem, slice};
  use std::path::PathBuf;
  use std::fs;

  static mut MOCKED_READER_SIZES: Vec<usize> = Vec::new();
  static mut MOCKED_READER_RETURNS: Vec<efi::Status> = Vec::new();
  extern "win64" fn test_mocked_reader(
      file_handle: *const core::ffi::c_void,
      file_offset: usize,
      read_size: *mut usize,
      output_buffer: *mut core::ffi::c_void
      ) -> efi::Status {
    unsafe {
      *read_size = MOCKED_READER_SIZES.remove(0);
      MOCKED_READER_RETURNS.remove(0)
    }
  }

  fn get_binary_test_file_path(file_name: &str) -> PathBuf {
    let mut binaries_path = PathBuf::from(".");
    binaries_path.push("tests");
    binaries_path.push("binaries");
    binaries_path.push(file_name);
    assert!(binaries_path.is_file(), "{} is not a valid binary file", file_name);
    binaries_path
  }

  extern "win64" fn test_file_reader(
      file_handle: *const core::ffi::c_void,
      file_offset: usize,
      read_size: *mut usize,
      output_buffer: *mut core::ffi::c_void
      ) -> efi::Status {
    if file_handle.is_null() || read_size.is_null() || output_buffer.is_null() {
      return efi::Status::INVALID_PARAMETER;
    }

    // NOTE:
    // All of this implementation is unsafe because the interface
    // design is unfixably broken. This lib should *not* provide this.
    // We're just going to replicated what the previous lib did.
    // let source_path = unsafe { *Box::from_raw(file_handle as *mut &str) };
    let source_file_name = unsafe { *(file_handle as *const &str) };
    let source = fs::read(get_binary_test_file_path(source_file_name)).unwrap();
    unsafe {
      let mut destination = slice::from_raw_parts_mut(output_buffer as *mut u8, *read_size);
      destination.copy_from_slice(&source[file_offset..file_offset+*read_size]);
    }

    efi::Status::SUCCESS
  }

  #[test]
  fn calling_from_raw_on_null_should_fail() {
    unsafe {
      assert!(PeCoffLoaderImageContext::from_raw(0 as *mut PeCoffLoaderImageContext).is_err());
    }
  }

  #[test]
  fn from_raw_should_require_an_image_read_fn() {
    let mut zero_buffer = vec![0; mem::size_of::<PeCoffLoaderImageContext>()];
    let raw_context_ptr = zero_buffer.as_mut_ptr() as *mut PeCoffLoaderImageContext;

    unsafe {
      assert!(PeCoffLoaderImageContext::from_raw(raw_context_ptr).is_err());
    }
  }

  #[test]
  fn from_raw_should_only_require_an_image_read_fn() {
    let mut zero_buffer = vec![0; mem::size_of::<PeCoffLoaderImageContext>()];
    let raw_context_ptr = zero_buffer.as_mut_ptr() as *mut PeCoffLoaderImageContext;

    unsafe {
      (*raw_context_ptr).image_read = test_mocked_reader;
      assert!(PeCoffLoaderImageContext::from_raw(raw_context_ptr).is_ok());
    }
  }

  #[test]
  fn from_raw_should_handle_bad_status() {
    let mut image_context = PeCoffLoaderImageContext::new(test_mocked_reader);
    let raw_context_ptr = &mut image_context as *mut PeCoffLoaderImageContext;

    unsafe {
      *(&mut (*raw_context_ptr).image_error as *mut PeCoffImageError as *mut u32) = (PeCoffImageError::ImageErrorUnsupported as u32) + 1;
      match PeCoffLoaderImageContext::from_raw(raw_context_ptr) {
        Ok(context) => {
          assert_eq!(context.image_error, PeCoffImageError::ImageErrorSuccess);
        },
        Err(_) => panic!("PeCoffLoaderImageContext::from_raw() should not have failed"),
      }
    }
  }

  #[test]
  fn failed_reads_should_return_err() {
    let mut image_context = PeCoffLoaderImageContext::new(test_mocked_reader);

    // Test 1, return an error.
    unsafe {
      MOCKED_READER_RETURNS.push(efi::Status::INVALID_PARAMETER);
      MOCKED_READER_SIZES.push(10);
    }
    assert!(image_context.read_image(0, 10).is_err());


    // Test 2, read too much data.
    unsafe {
      MOCKED_READER_RETURNS.push(efi::Status::SUCCESS);
      MOCKED_READER_SIZES.push(20);
    }
    let result = image_context.read_image(0, 10);
    assert!(result.is_err());
  }

  #[test]
  fn successful_reads_should_have_a_correct_size() {
    let mut image_context = PeCoffLoaderImageContext::new(test_mocked_reader);

    // Test 1, read less data.
    unsafe {
      MOCKED_READER_RETURNS.push(efi::Status::SUCCESS);
      MOCKED_READER_SIZES.push(5);
    }
   let result = image_context.read_image(10, 10);
    assert!(result.is_ok());
    assert_eq!(result.unwrap().len(), 5);

    // Test 2, read same data.
    unsafe {
      MOCKED_READER_RETURNS.push(efi::Status::SUCCESS);
      MOCKED_READER_SIZES.push(10);
    }
    let result = image_context.read_image(0, 10);
    assert!(result.is_ok());
    assert_eq!(result.unwrap().len(), 10);
  }

  #[test]
  fn update_info_from_headers_should_return_success_on_valid_image() {
    let mut image_context = PeCoffLoaderImageContext::new(test_file_reader);
    image_context.handle = &"RngDxe.efi" as *const &str as *const core::ffi::c_void;
    assert!(image_context.update_info_from_headers().is_ok());
  }
}
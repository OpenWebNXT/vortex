// Copyright © 2019-2023
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "mem.h"
#include <vector>
#include <iostream>
#include <fstream>
#include <assert.h>
#include "util.h"
#include <VX_config.h>
#include <bitset>

using namespace vortex;

uint64_t bits(uint64_t addr, uint8_t s_idx, uint8_t e_idx)
{
    return (addr >> s_idx) & ((1 << (e_idx - s_idx + 1)) - 1);
}

bool bit(uint64_t addr, uint8_t idx)
{
    return (addr) & (1 << idx);
}


RamMemDevice::RamMemDevice(const char *filename, uint32_t wordSize)
  : wordSize_(wordSize) {
  std::ifstream input(filename);

  if (!input) {
    std::cout << "Error reading file \"" << filename << "\" into RamMemDevice.\n";
    std::abort();
  }

  do {
    contents_.push_back(input.get());
  } while (input);

  while (contents_.size() & (wordSize-1)) {
    contents_.push_back(0x00);
  }
}

RamMemDevice::RamMemDevice(uint64_t size, uint32_t wordSize)
  : contents_(size)
  , wordSize_(wordSize)
{}

void RamMemDevice::read(void* data, uint64_t addr, uint64_t size) {
  auto addr_end = addr + size;
  if ((addr & (wordSize_-1))
   || (addr_end & (wordSize_-1))
   || (addr_end <= contents_.size())) {
    std::cout << "lookup of 0x" << std::hex << (addr_end-1) << " failed.\n";
    throw BadAddress();
  }

  const uint8_t *s = contents_.data() + addr;
  for (uint8_t *d = (uint8_t*)data, *de = d + size; d != de;) {
    *d++ = *s++;
  }
}

void RamMemDevice::write(const void* data, uint64_t addr, uint64_t size) {
  auto addr_end = addr + size;
  if ((addr & (wordSize_-1))
   || (addr_end & (wordSize_-1))
   || (addr_end <= contents_.size())) {
    std::cout << "lookup of 0x" << std::hex << (addr_end-1) << " failed.\n";
    throw BadAddress();
  }

  const uint8_t *s = (const uint8_t*)data;
  for (uint8_t *d = contents_.data() + addr, *de = d + size; d != de;) {
    *d++ = *s++;
  }
}

///////////////////////////////////////////////////////////////////////////////

void RomMemDevice::write(const void* /*data*/, uint64_t /*addr*/, uint64_t /*size*/) {
  std::cout << "attempt to write to ROM.\n";
  std::abort();
}

///////////////////////////////////////////////////////////////////////////////

bool MemoryUnit::ADecoder::lookup(uint64_t addr, uint32_t wordSize, mem_accessor_t* ma) {
  uint64_t end = addr + (wordSize - 1);
  assert(end >= addr);
  for (auto iter = entries_.rbegin(), iterE = entries_.rend(); iter != iterE; ++iter) {
    if (addr >= iter->start && end <= iter->end) {
      ma->md   = iter->md;
      ma->addr = addr - iter->start;
      return true;
    }
  }
  return false;
}

void MemoryUnit::ADecoder::map(uint64_t start, uint64_t end, MemDevice &md) {
  assert(end >= start);
  entry_t entry{&md, start, end};
  entries_.emplace_back(entry);
}

void MemoryUnit::ADecoder::read(void* data, uint64_t addr, uint64_t size) {
  // printf("====%s (addr= 0x%lx, size= 0x%lx) ====\n", __PRETTY_FUNCTION__,addr,size);
  mem_accessor_t ma;
  if (!this->lookup(addr, size, &ma)) {
    std::cout << "lookup of 0x" << std::hex << addr << " failed.\n";
    throw BadAddress();
  }
  ma.md->read(data, ma.addr, size);
}

void MemoryUnit::ADecoder::write(const void* data, uint64_t addr, uint64_t size) {
  // printf("====%s====\n", __PRETTY_FUNCTION__);
  mem_accessor_t ma;
  if (!this->lookup(addr, size, &ma)) {
    std::cout << "lookup of 0x" << std::hex << addr << " failed.\n";
    throw BadAddress();
  }
  ma.md->write(data, ma.addr, size);
}

///////////////////////////////////////////////////////////////////////////////

MemoryUnit::MemoryUnit(uint64_t pageSize)
  : pageSize_(pageSize)
  , enableVM_(pageSize != 0)
  , amo_reservation_({0x0, false}) 
#ifdef VM_ENABLE
  , TLB_HIT(0)
  , TLB_MISS(0)
  , TLB_EVICT(0)
  , PTW(0) {};
#else
  {
    if (pageSize != 0)
    {
      tlb_[0] = TLBEntry(0, 077);
    }
  }
#endif

void MemoryUnit::attach(MemDevice &m, uint64_t start, uint64_t end) {
  decoder_.map(start, end, m);
}

#ifdef VM_ENABLE
std::pair<bool, uint64_t> MemoryUnit::tlbLookup(uint64_t vAddr, ACCESS_TYPE type, uint64_t* size_bits) {
  // printf("====%s====\n", __PRETTY_FUNCTION__);
  
  //Find entry while accounting for different sizes.
  for (auto entry : tlb_)
  {
    if(entry.first == vAddr >> entry.second.size_bits)
    {
        *size_bits = entry.second.size_bits;
        vAddr = vAddr >> (*size_bits);
    }
  }

  
  auto iter = tlb_.find(vAddr);
  if (iter != tlb_.end()) {
    TLBEntry e = iter->second;

    //Set mru bit if it is a hit.
    iter->second.mru_bit = true;

    //If at full capacity and no other unset bits.
    // Clear all bits except the one we just looked up.
    if (tlb_.size() == TLB_SIZE)
    {
      // bool no_cleared = true;
      // for (auto& entry : tlb_)
      // {
      //   no_cleared = no_cleared & entry.second.mru_bit;
      // }

      // if(no_cleared)
      // {
        for (auto& entry : tlb_)
        {
          entry.second.mru_bit = false;
        }
        iter->second.mru_bit = true;
      //}
      
    }
    //Check access permissions.
    if ( (type == ACCESS_TYPE::FETCH) & ((e.r == 0) | (e.x == 0)) )
    {
      throw Page_Fault_Exception("Page Fault : Incorrect permissions.");
    }
    else if ( (type == ACCESS_TYPE::LOAD) & (e.r == 0) )
    {
      throw Page_Fault_Exception("Page Fault : Incorrect permissions.");
    }
    else if ( (type == ACCESS_TYPE::STORE) & (e.w == 0) )
    {
      throw Page_Fault_Exception("Page Fault : Incorrect permissions.");
    }
    else
    {
      //TLB Hit
      return std::make_pair(true, iter->second.pfn);
    }
  } else {
    //TLB Miss
    return std::make_pair(false, 0);
  }
}
#else
MemoryUnit::TLBEntry MemoryUnit::tlbLookup(uint64_t vAddr, uint32_t flagMask) {
  auto iter = tlb_.find(vAddr / pageSize_);
  if (iter != tlb_.end()) {
    if (iter->second.flags & flagMask)
      return iter->second;
    else {
      throw PageFault(vAddr, false);
    }
  } else {
    throw PageFault(vAddr, true);
  }
}

uint64_t MemoryUnit::toPhyAddr(uint64_t addr, uint32_t flagMask) {
  uint64_t pAddr;
  if (enableVM_) {
    TLBEntry t = this->tlbLookup(addr, flagMask);
    pAddr = t.pfn * pageSize_ + addr % pageSize_;
  } else {
    pAddr = addr;
  }
  return pAddr;
}
#endif

#ifdef VM_ENABLE
void MemoryUnit::read(void* data, uint64_t addr, uint32_t size, ACCESS_TYPE type) {
  // printf("====%s====\n", __PRETTY_FUNCTION__);
  uint64_t pAddr;
  pAddr = vAddr_to_pAddr(addr, type);
  return decoder_.read(data, pAddr, size);
}
#else
void MemoryUnit::read(void* data, uint64_t addr, uint32_t size, bool sup) {
  uint64_t pAddr = this->toPhyAddr(addr, sup ? 8 : 1);
  return decoder_.read(data, pAddr, size);
}
#endif
#ifdef VM_ENABLE
void MemoryUnit::write(const void* data, uint64_t addr, uint32_t size, ACCESS_TYPE type) {
  // printf("====%s====\n", __PRETTY_FUNCTION__);
  uint64_t pAddr;
  pAddr = vAddr_to_pAddr(addr, type);
  decoder_.write(data, pAddr, size);
  amo_reservation_.valid = false;
}
#else
void MemoryUnit::write(const void* data, uint64_t addr, uint32_t size, bool sup) {
  uint64_t pAddr = this->toPhyAddr(addr, sup ? 16 : 1);
  decoder_.write(data, pAddr, size);
  amo_reservation_.valid = false;
}
#endif

#ifdef VM_ENABLE
void MemoryUnit::amo_reserve(uint64_t addr) {
  uint64_t pAddr = this->vAddr_to_pAddr(addr,ACCESS_TYPE::LOAD);
  amo_reservation_.addr = pAddr;
  amo_reservation_.valid = true;
}
#else
void MemoryUnit::amo_reserve(uint64_t addr) {
  uint64_t pAddr = this->toPhyAddr(addr, 1);
  amo_reservation_.addr = pAddr;
  amo_reservation_.valid = true;
}
#endif

#ifdef VM_ENABLE
bool MemoryUnit::amo_check(uint64_t addr) {
  uint64_t pAddr = this->vAddr_to_pAddr(addr, ACCESS_TYPE::LOAD);
  return amo_reservation_.valid && (amo_reservation_.addr == pAddr);
}
#else
bool MemoryUnit::amo_check(uint64_t addr) {
  uint64_t pAddr = this->toPhyAddr(addr, 1);
  return amo_reservation_.valid && (amo_reservation_.addr == pAddr);
}
#endif


#ifdef VM_ENABLE

void MemoryUnit::tlbAdd(uint64_t virt, uint64_t phys, uint32_t flags, uint64_t size_bits) {
  // HW: evict TLB by Most Recently Used
  if (tlb_.size() == TLB_SIZE - 1) {
    for (auto& entry : tlb_)
    {
      entry.second.mru_bit = false;
    }
    
  } else if (tlb_.size() == TLB_SIZE) {
    uint64_t del;
    for (auto entry : tlb_) {
      if (!entry.second.mru_bit)
      {
        del = entry.first;
        break;
      }
    }
    tlb_.erase(tlb_.find(del));
    TLB_EVICT++;
  }
  tlb_[virt / pageSize_] = TLBEntry(phys / pageSize_, flags, size_bits);
}
#else

void MemoryUnit::tlbAdd(uint64_t virt, uint64_t phys, uint32_t flags) {
  tlb_[virt / pageSize_] = TLBEntry(phys / pageSize_, flags);
}
#endif

void MemoryUnit::tlbRm(uint64_t va) {
  if (tlb_.find(va / pageSize_) != tlb_.end())
    tlb_.erase(tlb_.find(va / pageSize_));
}

///////////////////////////////////////////////////////////////////////////////

void ACLManager::set(uint64_t addr, uint64_t size, int flags) {
  if (size == 0)
    return;

  uint64_t end = addr + size;

  // get starting interval
  auto it = acl_map_.lower_bound(addr);
  if (it != acl_map_.begin() && (--it)->second.end < addr) {
    ++it;
  }

  // Remove existing entries that overlap or are within the new range
  while (it != acl_map_.end() && it->first < end) {
    auto current = it++;
    uint64_t current_end = current->second.end;
    if (current_end <= addr)
      continue; // No overlap, no need to adjust

    // Adjust the current interval or erase it depending on overlap and flags
    if (current->first < addr) {
      if (current_end > end) {
        acl_map_[end] = {current_end, current->second.flags};
      }
      current->second.end = addr;
    } else {
      if (current_end > end) {
        acl_map_[end] = {current_end, current->second.flags};
      }
      acl_map_.erase(current);
    }
  }

  // Insert new range if flags are not zero
  if (flags != 0) {
    it = acl_map_.emplace(addr, acl_entry_t{end, flags}).first;
    // Merge adjacent ranges with the same flags
    auto prev = it;
    if (it != acl_map_.begin() && (--prev)->second.end == addr && prev->second.flags == flags) {
      prev->second.end = it->second.end;
      acl_map_.erase(it);
      it = prev;
    }
    auto next = std::next(it);
    if (next != acl_map_.end() && it->second.end == next->first && it->second.flags == next->second.flags) {
      it->second.end = next->second.end;
      acl_map_.erase(next);
    }
  }
}

bool ACLManager::check(uint64_t addr, uint64_t size, int flags) const {
  uint64_t end = addr + size;

  auto it = acl_map_.lower_bound(addr);
  if (it != acl_map_.begin() && (--it)->second.end < addr) {
    ++it;
  }

  while (it != acl_map_.end() && it->first < end) {
    if (it->second.end > addr) {
      if ((it->second.flags & flags) != flags) {
        std::cout << "Memory access violation from 0x" << std::hex << addr << " to 0x" << end << ", curent flags=" << it->second.flags << ", access flags=" << flags << std::endl;
        return false; // Overlapping entry is missing at least one required flag bit
      }
      addr = it->second.end; // Move to the end of the current matching range
    }
    ++it;
  }

  return true;
}

///////////////////////////////////////////////////////////////////////////////

RAM::RAM(uint64_t capacity, uint32_t page_size)
  : capacity_(capacity)
  , page_bits_(log2ceil(page_size))
  , last_page_(nullptr)
  , last_page_index_(0)
  , check_acl_(false) {
  assert(ispow2(page_size));
  if (capacity != 0) {
    assert(ispow2(capacity));
    assert(page_size <= capacity);
    assert(0 == (capacity % page_size));
  }
}

RAM::~RAM() {
  this->clear();
}

void RAM::clear() {
  for (auto& page : pages_) {
    delete[] page.second;
  }
}

uint64_t RAM::size() const {
  return uint64_t(pages_.size()) << page_bits_;
}

uint8_t *RAM::get(uint64_t address) const {
  if (capacity_ != 0 && address >= capacity_) {
    throw OutOfRange();
  }
  uint32_t page_size   = 1 << page_bits_;
  uint32_t page_offset = address & (page_size - 1);
  uint64_t page_index  = address >> page_bits_;

  uint8_t* page;
  if (last_page_ && last_page_index_ == page_index) {
    page = last_page_;
  } else {
    auto it = pages_.find(page_index);
    if (it != pages_.end()) {
      page = it->second;
    } else {
      uint8_t *ptr = new uint8_t[page_size];
      // set uninitialized data to "baadf00d"
      for (uint32_t i = 0; i < page_size; ++i) {
        ptr[i] = (0xbaadf00d >> ((i & 0x3) * 8)) & 0xff;
      }
      pages_.emplace(page_index, ptr);
      page = ptr;
    }
    last_page_ = page;
    last_page_index_ = page_index;
  }

  return page + page_offset;
}

void RAM::read(void* data, uint64_t addr, uint64_t size) {
  // printf("====%s (addr= 0x%lx, size= 0x%lx) ====\n", __PRETTY_FUNCTION__,addr,size);
  if (check_acl_ && acl_mngr_.check(addr, size, 0x1) == false) {
    throw BadAddress();
  }
  uint8_t* d = (uint8_t*)data;
  for (uint64_t i = 0; i < size; i++) {
    d[i] = *this->get(addr + i);
  }
}

void RAM::write(const void* data, uint64_t addr, uint64_t size) {
  if (check_acl_ && acl_mngr_.check(addr, size, 0x2) == false) {
    throw BadAddress();
  }
  const uint8_t* d = (const uint8_t*)data;
  for (uint64_t i = 0; i < size; i++) {
    *this->get(addr + i) = d[i];
  }
}

void RAM::set_acl(uint64_t addr, uint64_t size, int flags) {
  if (capacity_ != 0 && (addr + size)> capacity_) {
    throw OutOfRange();
  }
  acl_mngr_.set(addr, size, flags);
}

void RAM::loadBinImage(const char* filename, uint64_t destination) {
  std::ifstream ifs(filename);
  if (!ifs) {
    std::cout << "error: " << filename << " not found" << std::endl;
    std::abort();
  }

  ifs.seekg(0, ifs.end);
  size_t size = ifs.tellg();
  std::vector<uint8_t> content(size);
  ifs.seekg(0, ifs.beg);
  ifs.read((char*)content.data(), size);

  this->clear();
  this->write(content.data(), destination, size);
}

void RAM::loadHexImage(const char* filename) {
  auto hti = [&](char c)->uint32_t {
    if (c >= 'A' && c <= 'F')
      return c - 'A' + 10;
    if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
    return c - '0';
  };

  auto hToI = [&](const char *c, uint32_t size)->uint32_t {
    uint32_t value = 0;
    for (uint32_t i = 0; i < size; i++) {
      value += hti(c[i]) << ((size - i - 1) * 4);
    }
    return value;
  };

  std::ifstream ifs(filename);
  if (!ifs) {
    std::cout << "error: " << filename << " not found" << std::endl;
    std::abort();
  }

  ifs.seekg(0, ifs.end);
  size_t size = ifs.tellg();
  std::vector<char> content(size);
  ifs.seekg(0, ifs.beg);
  ifs.read(content.data(), size);

  uint32_t offset = 0;
  char *line = content.data();

  this->clear();

  while (true) {
    if (line[0] == ':') {
      uint32_t byteCount = hToI(line + 1, 2);
      uint32_t nextAddr = hToI(line + 3, 4) + offset;
      uint32_t key = hToI(line + 7, 2);
      switch (key) {
      case 0:
        for (uint32_t i = 0; i < byteCount; i++) {
          uint32_t addr  = nextAddr + i;
          uint32_t value = hToI(line + 9 + i * 2, 2);
          *this->get(addr) = value;
        }
        break;
      case 2:
        offset = hToI(line + 9, 4) << 4;
        break;
      case 4:
        offset = hToI(line + 9, 4) << 16;
        break;
      default:
        break;
      }
    }
    while (*line != '\n' && size != 0) {
      ++line;
      --size;
    }
    if (size <= 1)
      break;
    ++line;
    --size;
  }
}

#ifdef VM_ENABLE

bool MemoryUnit::need_trans(uint64_t dev_pAddr)
{
    // Check if the this is the BARE mode
    bool isBAREMode = (this->mode == VA_MODE::BARE);
    // Check if the address is reserved
    bool isReserved = (dev_pAddr >= PAGE_TABLE_BASE_ADDR);
    // Check if the address falls within the startup address range
    bool isStartAddress = (STARTUP_ADDR <= dev_pAddr) && (dev_pAddr < (STARTUP_ADDR + 0x40000));
    
    // Print the boolean results for debugging purposes
    // printf("0x%lx, %u, %u, %u \n", dev_pAddr,isBAREMode, isReserved, isStartAddress);

    // Return true if the address needs translation (i.e., it's not reserved and not a start address)
    return (!isBAREMode && !isReserved && !isStartAddress);
}

uint64_t MemoryUnit::vAddr_to_pAddr(uint64_t vAddr, ACCESS_TYPE type)
{
    uint64_t pfn;
    uint64_t size_bits;
    // printf("====%s====\n", __PRETTY_FUNCTION__);
    // printf("vaddr = 0x%lx, type = 0x%u\n",vAddr,type);
    if (!need_trans(vAddr))
    {
        // printf("Translation is not needed.\n");
        return vAddr;
    }

    //First lookup TLB.
    std::pair<bool, uint64_t> tlb_access = tlbLookup(vAddr, type,  &size_bits);
    if (tlb_access.first)
    {

        // printf("Found pfn %lx in TLB\n",tlb_access.second);
        pfn = tlb_access.second;
        TLB_HIT++;
    }
    else //Else walk the PT.
    {
        std::pair<uint64_t, uint8_t> ptw_access = page_table_walk(vAddr, type, &size_bits);
        tlbAdd(vAddr>>size_bits, ptw_access.first, ptw_access.second,size_bits);
        pfn = ptw_access.first; TLB_MISS++; PTW++;
        unique_translations.insert(vAddr>>size_bits);
        PERF_UNIQUE_PTW = unique_translations.size();

    }

    //Construct final address using pfn and offset.
    // std::cout << "[MemoryUnit] translated vAddr: 0x" << std::hex << vAddr << " to pAddr: 0x" << std::hex << ((pfn << size_bits) + (vAddr & ((1 << size_bits) - 1))) << std::endl;
    return (pfn << size_bits) + (vAddr & ((1 << size_bits) - 1));
}

std::pair<uint64_t, uint8_t> MemoryUnit::page_table_walk(uint64_t vAddr_bits, ACCESS_TYPE type, uint64_t* size_bits)
{   
    // printf("====%s====\n", __PRETTY_FUNCTION__);
    // printf("vaddr = 0x%lx, type = %u, size_bits %lu\n", vAddr_bits, type, *size_bits);
    uint64_t LEVELS = 2;
    vAddr_SV32_t vAddr(vAddr_bits);
    uint64_t pte_bytes = 0;

    //Get base page table.
    uint64_t pt_ba = this->ptbr << 12;
    int i = LEVELS - 1; 

    while(true)
    {

      //Read PTE.
      decoder_.read(&pte_bytes, pt_ba+vAddr.vpn[i]*PTE_SIZE, PTE_SIZE);
      PTE_SV32_t pte(pte_bytes);
      
      //Check if it has invalid flag bits.
      if ( (pte.v == 0) | ( (pte.r == 0) & (pte.w == 1) ) )
      {
        printf("Error: PTE FLAGS=0x%x\n",pte.flags);
        throw Page_Fault_Exception("Page Fault : Attempted to access invalid entry.");
      }

      if ( (pte.r == 0) & (pte.w == 0) & (pte.x == 0))
      {
        //Not a leaf node as rwx == 000
        i--;
        if (i < 0)
        {
          printf("Error: PTE FLAGS=0x%x\n",pte.flags);
          throw Page_Fault_Exception("Page Fault : No leaf node found.");
        }
        else
        {
          //Continue on to next level.
          pt_ba = (pte_bytes >> 10 ) << 12;
        }
      }
      else
      {
        //Leaf node found, finished walking.
        pt_ba = (pte_bytes >> 10 ) << 12;
        break;
      }
    }

    PTE_SV32_t pte(pte_bytes);

    //Check RWX permissions according to access type.
    if ( (type == ACCESS_TYPE::FETCH) & ((pte.r == 0) | (pte.x == 0)) )
    {
      printf("Error: PTE FLAGS=0x%x\n",pte.flags);
      throw Page_Fault_Exception("Page Fault : TYPE FETCH, Incorrect permissions.");
    }
    else if ( (type == ACCESS_TYPE::LOAD) & (pte.r == 0) )
    {
      printf("Error: PTE FLAGS=0x%x\n",pte.flags);
      throw Page_Fault_Exception("Page Fault : TYPE LOAD, Incorrect permissions.");
    }
    else if ( (type == ACCESS_TYPE::STORE) & (pte.w == 0) )
    {
      printf("Error: PTE FLAGS=0x%x\n",pte.flags);
      throw Page_Fault_Exception("Page Fault : TYPE STORE, Incorrect permissions.");
    }
    *size_bits = 12;
    uint64_t pfn = pt_ba >> *size_bits;
    return std::make_pair(pfn, pte_bytes & 0xff);
}


uint32_t MemoryUnit::get_satp()
{
  return satp;
}  
void MemoryUnit::set_satp(uint32_t satp)
{
  this->satp = satp;
  this->ptbr = satp & 0x003fffff; //22 bits
  this->mode = satp & 0x80000000 ? VA_MODE::SV32 : VA_MODE::BARE;
}
#endif
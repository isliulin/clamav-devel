//===- lib/MC/MCAsmStreamer.cpp - Text Assembly Output --------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "llvm/MC/MCStreamer.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/CodeGen/AsmPrinter.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCCodeEmitter.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCSectionMachO.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/raw_ostream.h"
using namespace llvm;

namespace {

class MCAsmStreamer : public MCStreamer {
  raw_ostream &OS;
  const MCAsmInfo &MAI;
  AsmPrinter *Printer;
  MCCodeEmitter *Emitter;
public:
  MCAsmStreamer(MCContext &Context, raw_ostream &_OS, const MCAsmInfo &tai,
                AsmPrinter *_Printer, MCCodeEmitter *_Emitter)
    : MCStreamer(Context), OS(_OS), MAI(tai), Printer(_Printer),
      Emitter(_Emitter) {}
  ~MCAsmStreamer() {}

  /// @name MCStreamer Interface
  /// @{

  virtual void SwitchSection(const MCSection *Section);

  virtual void EmitLabel(MCSymbol *Symbol);

  virtual void EmitAssemblerFlag(AssemblerFlag Flag);

  virtual void EmitAssignment(MCSymbol *Symbol, const MCExpr *Value);

  virtual void EmitSymbolAttribute(MCSymbol *Symbol, SymbolAttr Attribute);

  virtual void EmitSymbolDesc(MCSymbol *Symbol, unsigned DescValue);

  virtual void EmitCommonSymbol(MCSymbol *Symbol, unsigned Size,
                                unsigned ByteAlignment);

  virtual void EmitZerofill(const MCSection *Section, MCSymbol *Symbol = 0,
                            unsigned Size = 0, unsigned ByteAlignment = 0);

  virtual void EmitBytes(const StringRef &Data);

  virtual void EmitValue(const MCExpr *Value, unsigned Size);

  virtual void EmitValueToAlignment(unsigned ByteAlignment, int64_t Value = 0,
                                    unsigned ValueSize = 1,
                                    unsigned MaxBytesToEmit = 0);

  virtual void EmitValueToOffset(const MCExpr *Offset,
                                 unsigned char Value = 0);
  
  virtual void EmitInstruction(const MCInst &Inst);

  virtual void Finish();
  
  /// @}
};

} // end anonymous namespace.

/// Allow printing symbols directly to a raw_ostream with proper quoting.
static inline raw_ostream &operator<<(raw_ostream &os, const MCSymbol *S) {
  S->print(os);
  return os;
}

/// Allow printing values directly to a raw_ostream.
static inline raw_ostream &operator<<(raw_ostream &os, const MCExpr &Value) {
  Value.print(os);
  return os;
}

static inline int64_t truncateToSize(int64_t Value, unsigned Bytes) {
  assert(Bytes && "Invalid size!");
  return Value & ((uint64_t) (int64_t) -1 >> (64 - Bytes * 8));
}

static inline const MCExpr *truncateToSize(const MCExpr *Value,
                                           unsigned Bytes) {
  // FIXME: Do we really need this routine?
  return Value;
}

void MCAsmStreamer::SwitchSection(const MCSection *Section) {
  assert(Section && "Cannot switch to a null section!");
  if (Section != CurSection) {
    CurSection = Section;
    Section->PrintSwitchToSection(MAI, OS);
  }
}

void MCAsmStreamer::EmitLabel(MCSymbol *Symbol) {
  assert(Symbol->isUndefined() && "Cannot define a symbol twice!");
  assert(CurSection && "Cannot emit before setting section!");

  OS << Symbol << ":\n";
  Symbol->setSection(*CurSection);
}

void MCAsmStreamer::EmitAssemblerFlag(AssemblerFlag Flag) {
  switch (Flag) {
  default: assert(0 && "Invalid flag!");
  case SubsectionsViaSymbols: OS << ".subsections_via_symbols"; break;
  }
  OS << '\n';
}

void MCAsmStreamer::EmitAssignment(MCSymbol *Symbol, const MCExpr *Value) {
  // Only absolute symbols can be redefined.
  assert((Symbol->isUndefined() || Symbol->isAbsolute()) &&
         "Cannot define a symbol twice!");

  OS << Symbol << " = " << *Value << '\n';
}

void MCAsmStreamer::EmitSymbolAttribute(MCSymbol *Symbol, 
                                        SymbolAttr Attribute) {
  switch (Attribute) {
  case Global: OS << ".globl"; break;
  case Hidden: OS << ".hidden"; break;
  case IndirectSymbol: OS << ".indirect_symbol"; break;
  case Internal: OS << ".internal"; break;
  case LazyReference: OS << ".lazy_reference"; break;
  case NoDeadStrip: OS << ".no_dead_strip"; break;
  case PrivateExtern: OS << ".private_extern"; break;
  case Protected: OS << ".protected"; break;
  case Reference: OS << ".reference"; break;
  case Weak: OS << ".weak"; break;
  case WeakDefinition: OS << ".weak_definition"; break;
  case WeakReference: OS << ".weak_reference"; break;
  }

  OS << ' ' << Symbol << '\n';
}

void MCAsmStreamer::EmitSymbolDesc(MCSymbol *Symbol, unsigned DescValue) {
  OS << ".desc" << ' ' << Symbol << ',' << DescValue << '\n';
}

void MCAsmStreamer::EmitCommonSymbol(MCSymbol *Symbol, unsigned Size,
                                     unsigned ByteAlignment) {
  OS << ".comm";
  OS << ' ' << Symbol << ',' << Size;
  if (ByteAlignment != 0)
    OS << ',' << Log2_32(ByteAlignment);
  OS << '\n';
}

void MCAsmStreamer::EmitZerofill(const MCSection *Section, MCSymbol *Symbol,
                                 unsigned Size, unsigned ByteAlignment) {
  // Note: a .zerofill directive does not switch sections.
  OS << ".zerofill ";
  
  // This is a mach-o specific directive.
  const MCSectionMachO *MOSection = ((const MCSectionMachO*)Section);
  OS << MOSection->getSegmentName() << "," << MOSection->getSectionName();
  
  if (Symbol != NULL) {
    OS << ',' << Symbol << ',' << Size;
    if (ByteAlignment != 0)
      OS << ',' << Log2_32(ByteAlignment);
  }
  OS << '\n';
}

void MCAsmStreamer::EmitBytes(const StringRef &Data) {
  assert(CurSection && "Cannot emit contents before setting section!");
  for (unsigned i = 0, e = Data.size(); i != e; ++i)
    OS << ".byte " << (unsigned) (unsigned char) Data[i] << '\n';
}

void MCAsmStreamer::EmitValue(const MCExpr *Value, unsigned Size) {
  assert(CurSection && "Cannot emit contents before setting section!");
  // Need target hooks to know how to print this.
  switch (Size) {
  default:
    llvm_unreachable("Invalid size for machine code value!");
  case 1: OS << ".byte"; break;
  case 2: OS << ".short"; break;
  case 4: OS << ".long"; break;
  case 8: OS << ".quad"; break;
  }

  OS << ' ' << *truncateToSize(Value, Size) << '\n';
}

void MCAsmStreamer::EmitValueToAlignment(unsigned ByteAlignment, int64_t Value,
                                         unsigned ValueSize,
                                         unsigned MaxBytesToEmit) {
  // Some assemblers don't support non-power of two alignments, so we always
  // emit alignments as a power of two if possible.
  if (isPowerOf2_32(ByteAlignment)) {
    switch (ValueSize) {
    default: llvm_unreachable("Invalid size for machine code value!");
    case 1: OS << MAI.getAlignDirective(); break;
    // FIXME: use MAI for this!
    case 2: OS << ".p2alignw "; break;
    case 4: OS << ".p2alignl "; break;
    case 8: llvm_unreachable("Unsupported alignment size!");
    }
    
    if (MAI.getAlignmentIsInBytes())
      OS << ByteAlignment;
    else
      OS << Log2_32(ByteAlignment);

    if (Value || MaxBytesToEmit) {
      OS << ", " << truncateToSize(Value, ValueSize);

      if (MaxBytesToEmit) 
        OS << ", " << MaxBytesToEmit;
    }
    OS << '\n';
    return;
  }
  
  // Non-power of two alignment.  This is not widely supported by assemblers.
  // FIXME: Parameterize this based on MAI.
  switch (ValueSize) {
  default: llvm_unreachable("Invalid size for machine code value!");
  case 1: OS << ".balign";  break;
  case 2: OS << ".balignw"; break;
  case 4: OS << ".balignl"; break;
  case 8: llvm_unreachable("Unsupported alignment size!");
  }

  OS << ' ' << ByteAlignment;
  OS << ", " << truncateToSize(Value, ValueSize);
  if (MaxBytesToEmit) 
    OS << ", " << MaxBytesToEmit;
  OS << '\n';
}

void MCAsmStreamer::EmitValueToOffset(const MCExpr *Offset,
                                      unsigned char Value) {
  // FIXME: Verify that Offset is associated with the current section.
  OS << ".org " << *Offset << ", " << (unsigned) Value << '\n';
}

void MCAsmStreamer::EmitInstruction(const MCInst &Inst) {
  assert(CurSection && "Cannot emit contents before setting section!");

  // If we have an AsmPrinter, use that to print.
  if (Printer) {
    Printer->printMCInst(&Inst);

    // Show the encoding if we have a code emitter.
    if (Emitter) {
      SmallString<256> Code;
      raw_svector_ostream VecOS(Code);
      Emitter->EncodeInstruction(Inst, VecOS);
      VecOS.flush();
  
      OS.indent(20);
      OS << " # encoding: [";
      for (unsigned i = 0, e = Code.size(); i != e; ++i) {
        if (i)
          OS << ',';
        OS << format("%#04x", uint8_t(Code[i]));
      }
      OS << "]\n";
    }

    return;
  }

  // Otherwise fall back to a structural printing for now. Eventually we should
  // always have access to the target specific printer.
  Inst.print(OS);
  OS << '\n';
}

void MCAsmStreamer::Finish() {
  OS.flush();
}
    
MCStreamer *llvm::createAsmStreamer(MCContext &Context, raw_ostream &OS,
                                    const MCAsmInfo &MAI, AsmPrinter *AP,
                                    MCCodeEmitter *CE) {
  return new MCAsmStreamer(Context, OS, MAI, AP, CE);
}
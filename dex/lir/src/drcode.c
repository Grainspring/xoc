/*@
XOC Release License

Copyright (c) 2013-2014, Alibaba Group, All rights reserved.

    compiler@aliexpress.com

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the Su Zhenyu nor the names of its contributors
      may be used to endorse or promote products derived from this software
      without specific prior written permission.

THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS
BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

author: GongKai, JinYue
@*/
#include "libdex/DexFile.h"
#include "libdex/DexClass.h"
#include "libdex/DexCatch.h"
#include "libdex/InstrUtils.h"
#include "libdex/DexProto.h"
#include "libdex/CmdUtils.h"

#include "dir.h"
#include "liropcode.h"
#include <assert.h>
#include <stdio.h>

#include "drAlloc.h"
#include "d2lcode.h"
#include "d2d_l2d.h"
#include "d2d_d2l.h"
#include "utils/cbytestream.h"
#include "xassert.h"
#include "lircomm.h"
#include "lir.h"
#include "anainterface.h"
#include "dex_driver.h"

#ifdef __cplusplus
extern "C" {
#endif
    LIRCode* anaEntry(LIRCode* lirCode);
#ifdef __cplusplus
}
#endif


Int32 gMemAlloc = 0;

int isExecuteInlineMethod(const DexFile* pDexFile, DecodedInstruction* pDecInsn);

typedef struct {
    UInt32     posNum;
    UInt32* posMap;
}PositionMap;

static bool isObjectInit(const DexFile* pDexFile, DIRDecodedInsn* pDecInsn){
    const DexMethodId* pMethodId;
    u4 methodIdx;
    const char* className;

    if(pDecInsn->opcode != OP_INVOKE_DIRECT)
        return false;

    methodIdx = pDecInsn->vB;
    pMethodId = dexGetMethodId(pDexFile, methodIdx);
    className = dexStringByTypeIdx(pDexFile,pMethodId->classIdx);
    if(strcmp("Ljava/lang/Object;",className) == 0){
        const char* methodName;

        methodName = dexStringById(pDexFile,pMethodId->nameIdx);
        if(strcmp("<init>",methodName) == 0)
            return true;
    }

    return false;
}

static inline bool contentIsInsn(const UInt16 *codePtr) {
    UInt16 instr = *codePtr;
    DIROpcode opcode = (DIROpcode)(instr & 0xff);

    return (opcode != 0 || instr == 0);
}

static inline DIROpcode getOpcodeFromCodeUnit(UInt16 codeUnit) {
    int lowByte = codeUnit & 0xff;
    if (lowByte != 0xff) {
        return (DIROpcode) lowByte;
    } else {
        return (DIROpcode) ((codeUnit >> 8) | 0x100);
    }
}

Int32 findPos(PositionMap* posMap,UInt32 target){
    UInt32 low;
    UInt32 mid;
    UInt32 high;
    UInt32 tmp;
    UInt32* map = posMap->posMap;
    UInt32  num = posMap->posNum;

    low = 0;
    high = num;
    while(true) {
        mid = (low + high) >> 1;
        tmp = map[mid];
        if(tmp == target) {
            return mid;
        }
        if(mid == low)
            break;
        if(target < tmp)
            high = mid;
        else
            low = mid;
    }

    abort();
    return -1;
}

void genInstruction(const DexFile* pDexFile, UInt16 *codeStart, UInt16 *codeEnd, LIRBaseOp** lirList, PositionMap* posMap){
    UInt32 instrIdx = 0;
    UInt16* codePtr = codeStart;

    UInt32 dexOffset = 0;

    while (codePtr < codeEnd) {
        LIRBaseOp* result;

        if (!contentIsInsn(codePtr)) {
            break;
        }
        UInt16 instr = *codePtr;
        DIROpcode opcode = getOpcodeFromCodeUnit(instr);
        UInt8 lirOpcode = gDIR2LIRInfo.opcodes[opcode];
        UInt8 flags = gDIR2LIRInfo.flags[opcode];
        UInt32 formats = gLIROpcodeInfo.formats[lirOpcode];
        DIRDecodedInsn dInsn;
        memset(&dInsn,0,sizeof(DIRDecodedInsn));
        DIRDecodeInstruction(codePtr,&dInsn);

        switch(formats){
        case lirFmtV:
        {
            LIRBaseOp* lir =  (LIRBaseOp*)LIRMALLOC(sizeof(LIRBaseOp));
            lir->opcode = lirOpcode;
            lir->flags = flags;

            result = (LIRBaseOp*)lir;
            break;
        }
        case lirFmtA:
        case lirFmtR:
        {
            LIRAOp* lir = (LIRAOp*)LIRMALLOC(sizeof(LIRAOp));
            lir->opcode = lirOpcode;
            lir->flags = flags;
            //value or dst
            lir->vA = dInsn.vA;

            result = (LIRBaseOp*)lir;
            break;
        }
        case lirFmtT://goto
        {
            LIRGOTOOp* lir = (LIRGOTOOp*)LIRMALLOC(sizeof(LIRGOTOOp));
            lir->opcode = lirOpcode;
            lir->flags = flags;

            UInt32 target;
            //value or dst
            if(opcode == OP_GOTO){
                target = dexOffset +((Int8)dInsn.vA);
            }else if(opcode == OP_GOTO_16){
                target = dexOffset +((Int16)dInsn.vA);
            }else{
                target = dexOffset +((Int32)dInsn.vA);
            }
            Int32 instrNum = findPos(posMap,target);
            lir->target = instrNum;

            result = (LIRBaseOp*)lir;
            break;
        }
        case lirFmtRA:
        {
            LIRABOp* lir = (LIRABOp*)LIRMALLOC(sizeof(LIRABOp));
            lir->opcode = lirOpcode;
            lir->flags = flags;
            //value or dst
            lir->vA = dInsn.vA;
            lir->vB = dInsn.vB;

            result = (LIRBaseOp*)lir;
            break;
        }
        case lirFmtRS:
        case lirFmtAS:
        {
            LIRABOp* lir = (LIRABOp*)LIRMALLOC(sizeof(LIRABOp));
            lir->opcode = lirOpcode;
            lir->flags = flags;
            //value or dst
            lir->vA = dInsn.vA;
            lir->vB = dInsn.vB;

            result = (LIRBaseOp*)lir;
            break;
        }
        case lirFmtAT:
        {
            LIRABOp* lir = (LIRABOp*)LIRMALLOC(sizeof(LIRABOp));
            lir->opcode = lirOpcode;
            lir->flags = flags;
            //value or dst
            lir->vA = dInsn.vA;
            UInt32 target = dexOffset +((Int16)dInsn.vB);
            Int32 instrNum = findPos(posMap,target);
            lir->vB = instrNum;

            result = (LIRBaseOp*)lir;
            break;
        }
        case lirFmtRL://LOP_CONST
        {
            LIRConstOp* lir = (LIRConstOp*)LIRMALLOC(sizeof(LIRConstOp));
            lir->opcode = lirOpcode;
            lir->flags = flags;
            //value or dst
            lir->vA = dInsn.vA;

            if(opcode == OP_CONST_HIGH16){
                lir->vB = (UInt32)(dInsn.vB << 16);
            }else if(opcode == OP_CONST_WIDE_HIGH16){
                Int64 data = (Int16)dInsn.vB;
                lir->vB = (Int64)(data << 48);
            }else if(opcode == OP_CONST_WIDE){
                lir->vB = dInsn.vB_wide;
            }else if(flags == LIR_JDT_wide){
                lir->vB = (Int32)dInsn.vB;
            }else{
                lir->vB = (UInt32)dInsn.vB;
            }

            result = (LIRBaseOp*)lir;
            break;
        }
        case lirFmtABC:
        case lirFmtRAB:
        case lirFmtRAL:
        {
            LIRABCOp* lir = (LIRABCOp*)LIRMALLOC(sizeof(LIRABCOp));
            lir->opcode = lirOpcode;
            lir->flags = flags;
            //value or dst
            lir->vA = dInsn.vA;
            lir->vB = dInsn.vB;
            lir->vC = dInsn.vC;

            result = (LIRBaseOp*)lir;
            break;
        }
        case lirFmtABS:
        case lirFmtRAS:
        {
            LIRABCOp* lir = (LIRABCOp*)LIRMALLOC(sizeof(LIRABCOp));
            lir->opcode = lirOpcode;
            lir->flags = flags;
            //value or dst
            lir->vA = dInsn.vA;
            lir->vB = dInsn.vB;
            lir->vC = dInsn.vC;
            result = (LIRBaseOp*)lir;
            break;
        }
        case lirFmtABT:
        {
            LIRABCOp* lir = (LIRABCOp*)LIRMALLOC(sizeof(LIRABCOp));
            lir->opcode = lirOpcode;
            lir->flags = flags;
            //value or dst
            lir->vA = dInsn.vA;
            lir->vB = dInsn.vB;

            UInt32 target = dexOffset +((Int16)dInsn.vC);
            Int32 instrNum = findPos(posMap,target);
            lir->vC = instrNum;

            result = (LIRBaseOp*)lir;
            break;
        }
        case lirFmtSWITCH:
        {
            UInt32 dataSize;
            UInt16* data;

            LIRSwitchOp* lir = (LIRSwitchOp*)LIRMALLOC(sizeof(LIRSwitchOp));
            lir->opcode = lirOpcode;
            lir->flags = flags;
            lir->value = dInsn.vA;

            data = codePtr + dInsn.vB;

            switch(lirOpcode){
            case LOP_TABLE_SWITCH:{
                assert(data[0] == 0x0100);
                UInt32 i;
                Int32* target;
                UInt32* lirTarget;
                UInt32 size = data[1];

                dataSize = 8 + size * 4;
                lir->data = (UInt16*)LIRMALLOC(dataSize);
                memcpy(lir->data,(BYTE*)data,8);

                target = (Int32*)(((BYTE*)data) + 8);
                lirTarget = (UInt32*)(((BYTE*)lir->data) + 8);
                for(i = 0; i < size; i++){
                    UInt32 addr = dexOffset + (Int32)target[i];
                    Int32 instrNum = findPos(posMap,addr);
                    lirTarget[i] = (UInt32)instrNum;
                }
                break;
            }
            case LOP_LOOKUP_SWITCH:{
                assert(data[0] == 0x0200);
                UInt32 i,copySize;
                Int32 *target;
                UInt32 *lirTarget;
                UInt32 size = data[1];

                dataSize = 4 + size * 8;
                lir->data = (UInt16*)LIRMALLOC(dataSize);
                copySize = 4 + size*4;
                memcpy(lir->data,(BYTE*)data,copySize);

                target = (Int32*)(((BYTE*)data) + copySize);
                lirTarget = (UInt32*)(((BYTE*)lir->data) + copySize);
                for(i = 0; i < size; i++){
                    UInt32 addr = dexOffset + (Int32)target[i];
                    Int32 instrNum = findPos(posMap,addr);
                    lirTarget[i] = (UInt32)instrNum;
                }

                break;
            }
            case LOP_FILL_ARRAY_DATA:{
                assert(data[0] == 0x0300);
                UInt16 elemWidth = data[1];
                UInt32 len = data[2] | (((UInt32)data[3]) << 16);
                dataSize = (4 + (elemWidth * len + 1)/2) * 2;
                lir->data = (UInt16*)LIRMALLOC(dataSize);
                memcpy(lir->data,(BYTE*)data,dataSize);
                break;
            }
            default:{
                abort();
            }
            }

            result = (LIRBaseOp*)lir;
            break;
        }
        case lirFmtINVOKE:
        {
            LIRInvokeOp* lir = (LIRInvokeOp*)LIRMALLOC(sizeof(LIRInvokeOp));
            const DexMethodId* method = NULL;
            const DexProtoId* proto = NULL;
            const char* shorty = NULL;

            lir->flags = flags;
            /* TODO to process the objectinit and excute inline
            if(isObjectInit(pDexFile, &dInsn))
            {
                lir->opcode = LOP_NOP;
                goto END;
            } else {
                Int32 idx = isExecuteInlineMethod(pDexFile, (DecodedInstruction*)(&dInsn));
                if(idx != -1)
                {
                    lir->flags = flags | LIR_invoke_inline;
                    lir->exeRef = idx;
                }
                else
                    lir->flags = flags;
            }
            */

            lir->method = (void*)dInsn.vB;
            lir->argc = dInsn.vA;
            lir->opcode = lirOpcode;

            method = dexGetMethodId(pDexFile, (UInt32)(ULong)lir->method);
            proto = dexGetProtoId(pDexFile, method->protoIdx);
            shorty = strdup(dexStringById(pDexFile, proto->shortyIdx));
            lir->shorty = shorty;
            lir->args = (UInt16*)LIRMALLOC(lir->argc*sizeof(UInt32));

           if((flags&0xf0) == LIR_Range){
                int i;
                for(i = 0; i < lir->argc; i++){
                    lir->args[i] =     dInsn.vC + i;
                }
            }else{
                int i;
                for(i = 0; i < lir->argc; i++){
                    lir->args[i] =     dInsn.arg[i];
                }
            }
END:
            result = (LIRBaseOp*)lir;
            break;
        }default:{
            result = NULL;
            abort();
        }
        }

        lirList[instrIdx] = result;
        UInt32 width = gDIROpcodeInfo.widths[opcode];
        codePtr += width;
        dexOffset += width;
        instrIdx ++;
    }
}

static void genTryCatches(const DexFile* pDexFile, const DexCode* pCode, PositionMap* posMap, LIRCode* code)
{
    const DexTry* pTries = dexGetTries(pCode);
    UInt32 triesSize = pCode->triesSize;
    UInt32 i;

    LIROpcodeTry* trys = (LIROpcodeTry*)LIRMALLOC(triesSize*sizeof(LIROpcodeTry));
    code->triesSize = triesSize;
    code->trys = trys;

    for (i = 0; i < triesSize; i++) {
        const DexTry* pTry = &pTries[i];
        UInt32 start = pTry->startAddr;
        UInt32 end = start + pTry->insnCount;
        UInt32 newStart;
        UInt32 newEnd;
        UInt32 handlerSize;
        DexCatchIterator iterator;

        newStart = findPos(posMap,start);
        newEnd = findPos(posMap,end);

        LIROpcodeTry* _try = trys + i;

        _try->start_pc = newStart;
        _try->end_pc = newEnd;

        dexCatchIteratorInit(&iterator, pCode, pTry->handlerOff);
        handlerSize = 0;
        while(true){
            DexCatchHandler* handler = dexCatchIteratorNext(&iterator);
            if (handler == NULL) {
                break;
            }
            handlerSize ++;
        }

        _try->catchSize = handlerSize;
        _try->catches = (LIROpcodeCatch*)LIRMALLOC(handlerSize*sizeof(LIROpcodeCatch));

        dexCatchIteratorInit(&iterator, pCode, pTry->handlerOff);
        UInt32 idx = 0;
        while(true){
            DexCatchHandler* handler = dexCatchIteratorNext(&iterator);
            UInt32 newHandler;

            if (handler == NULL) {
                break;
            }

            LIROpcodeCatch* _catch = _try->catches + idx;
            newHandler = findPos(posMap,handler->address);

            _catch->handler_pc = newHandler;

            if(handler->typeIdx == kDexNoIndex){
                _catch->class_type = 0x00;  //TODO may a bug
            }else{
                _catch->class_type = (UInt32)handler->typeIdx;
            }
            idx++;
        }
    }
}

static Int32 l2dWithAot(D2Dpool* pool, const DexCode* pCode, LIRCode* code)
{
    Int32 err = 0;
    code = anaEntry(code);
    lir2dexCode(pool, pCode, code);

    return err;
}

#ifdef COMPILE_DEX2LEX
bool aotDrGenCode(const DexFile* pDexFile,  DexMethod* pDexMethod, LCodeData* codeData){
    const DexCode* pCode = dexGetCode(pDexFile, pDexMethod);
    UInt16* codeStart = (UInt16*)pCode->insns;
    UInt16* codeEnd = codeStart + pCode->insnsSize;
    UInt16* codePtr = codeStart;

    /*init mem heap*/
    drLinearInit();

    LIRCode* code = (LIRCode*)LIRMALLOC(sizeof(LIRCode));

    //const DexMethodId* method = dexGetMethodId(pDexFile, pDexMethod->methodIdx);
     LIRBaseOp** lirList;

    UInt32 instrCount = 0;
    PositionMap positionMap;

    /*positionMap may bigger than we need.*/
    positionMap.posMap = (UInt32*)malloc((codeEnd - codeStart + 1)*sizeof(UInt32));

    UInt32 dexOffset = 0;
    while (codePtr < codeEnd) {
        if (!contentIsInsn(codePtr)) {
            break;
        }
        UInt16 instr = *codePtr;
        DIROpcode opcode = getOpcodeFromCodeUnit(instr);
        UInt32 width = gDIROpcodeInfo.widths[opcode];

        positionMap.posMap[instrCount] = dexOffset;

        codePtr += width;
        dexOffset += width;

        instrCount++;
    }

    positionMap.posMap[instrCount] = dexOffset;
    positionMap.posNum = instrCount + 1;

    lirList = (LIRBaseOp**)LIRMALLOC(instrCount * sizeof(LIRBaseOp*));

    memset(lirList, 0 ,instrCount*sizeof(LIRBaseOp*));

    code->instrCount = instrCount;
    code->lirList = lirList;
    code->maxVars = pCode->registersSize;

    //gen instruction
    genInstruction(pDexFile, codeStart, codeEnd, lirList, &positionMap);

    //ge try catch
    genTryCatches(pDexFile,pCode,&positionMap,code);

    free(positionMap.posMap);

    const DexMethodId* pMethodId;
    pMethodId = dexGetMethodId(pDexFile, pDexMethod->methodIdx);

    code->numArgs = pCode->insSize;
    code->flags = 0;

    //tmp impl
    code->strClass = strdup(dexStringByTypeIdx(pDexFile, pMethodId->classIdx));
    code->strName = strdup(dexStringById(pDexFile, pMethodId->nameIdx));

    const DexProtoId* proto = dexGetProtoId(pDexFile, pMethodId->protoIdx);
    const char* shorty = strdup(dexStringById(pDexFile, proto->shortyIdx));
    code->shortName = shorty;

    if((ACC_STATIC & pDexMethod->accessFlags))
        code->flags |= LIR_FLAGS_ISSTATIC;

    /*analyse the lir to make it better,
     *and transform lir to lex*/
    lir2lexTransform(code, codeData);

    /*free heap*/
    drLinearFree();
    return true;
}
#endif

static bool isInArray(char const** array, char const* n, int size)
{
    for (int i = 0; i < size; i++) {
        if (strstr(array[i], n) != 0) {
            return true;
        }
    }
    return false;
}


#define CAFF_COUNT 20
#define BECHMARK_PI_COUNT 54

static int gcount = 0;
static bool do_ana(DexFile * df, DexMethod const* dm)
{
    DexMethodId const* MethodId = dexGetMethodId(df, dm->methodIdx);
    char const* cname = dexStringByTypeIdx(df, MethodId->classIdx);
    char const* mname = dexStringById(df, MethodId->nameIdx);

    //char * buf = (char*)alloca(strlen(cname) + strlen(mname) + 10);
    //sprintf(buf, "%s::%s", cname, mname);

    if (isInArray(caffcls, cname, caffnum)) {
        return true;
    }
    return false;
}


bool d2rMethod(D2Dpool* pool, DexFile* pDexFile, const DexMethod* pDexMethod)
{
    const DexCode* pCode = dexGetCode(pDexFile, pDexMethod);
    UInt16* codeStart = (UInt16*)pCode->insns;
    UInt16* codeEnd = codeStart + pCode->insnsSize;
    UInt16* codePtr = codeStart;

    drLinearInit();

    LIRCode* code = (LIRCode*)LIRMALLOC(sizeof(LIRCode));
     LIRBaseOp** lirList;

    UInt32 instrCount = 0;
    PositionMap positionMap;

    /*positionMap may bigger than we need.*/
    positionMap.posMap = (UInt32*)malloc((codeEnd - codeStart + 1)*sizeof(UInt32));
    UInt32 dexOffset = 0;

    while (codePtr < codeEnd) {
        if (!contentIsInsn(codePtr)) {
            break;
        }

        UInt16 instr = *codePtr;
        DIROpcode opcode = getOpcodeFromCodeUnit(instr);
        UInt32 width = gDIROpcodeInfo.widths[opcode];

        positionMap.posMap[instrCount] = dexOffset;

        codePtr += width;
        dexOffset += width;
        instrCount++;
    }

    positionMap.posMap[instrCount] = dexOffset;
    positionMap.posNum = instrCount + 1;

    lirList = (LIRBaseOp**)LIRMALLOC(instrCount * sizeof(LIRBaseOp*));
    memset(lirList, 0 ,instrCount*sizeof(LIRBaseOp*));

    code->instrCount = instrCount;
    code->lirList = lirList;
    code->maxVars = pCode->registersSize;

    const DexMethodId* pMethodId;
    pMethodId = dexGetMethodId(pDexFile, pDexMethod->methodIdx);

    code->numArgs = pCode->insSize;
    code->flags = 0;

    //tmp impl
    code->strClass = dexStringByTypeIdx(pDexFile, pMethodId->classIdx);
    code->strName = dexStringById(pDexFile, pMethodId->nameIdx);

    const DexProtoId* proto = dexGetProtoId(pDexFile, pMethodId->protoIdx);
    const char* shorty = dexStringById(pDexFile, proto->shortyIdx);
    code->shortName = shorty;

    //gen instruction
    genInstruction(pDexFile, codeStart, codeEnd, lirList, &positionMap);
    //ge try catch
    genTryCatches(pDexFile,pCode,&positionMap,code);

    free(positionMap.posMap);

    if((ACC_STATIC & pDexMethod->accessFlags))
        code->flags |= LIR_FLAGS_ISSTATIC;

    compileFunc(pool, code, pDexFile, pDexMethod);
    lir2dexCode(pool, pCode, code);

    //Leave it to verify.
    //lir2dexCode_orig(pool, pCode, code);

    //Obsolete
    //l2dWithAot(pool, pCode, code);
    drLinearFree();
    return true;
}

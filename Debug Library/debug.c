/***************************************************************
                            debug.c
                               
Allows communication between the N64 cartridge and the PC using
UNFLoader.
https://github.com/buu342/N64-UNFLoader
***************************************************************/

#include <ultra64.h> 
#include <stdarg.h> // Located in ultra\GCC\MIPSE\INCLUDE
#include <PR/os_internal.h> // Needed for Crash's Linux toolchain
#include <string.h> // Needed for Crash's Linux toolchain
#include "debug.h"


#if DEBUG_MODE

/*********************************
       Standard Definitions
*********************************/

// Cart definitions
#define CART_NONE       0
#define CART_64DRIVE    2
#define CART_EVERDRIVE3 3
#define CART_EVERDRIVE7 4

// Data types defintions
#define DATATYPE_TEXT       0x01
#define DATATYPE_RAWBINARY  0x02
#define DATATYPE_SCREENSHOT 0x03


/*********************************
          64Drive macros
*********************************/

// Cartridge Interface definitions. Obtained from 64Drive's Spec Sheet
#define D64_BASE_ADDRESS   0xB0000000
#define D64_CIREG_ADDRESS  0x08000000
#define D64_CIBASE_ADDRESS 0xB8000000
#define D64_DEBUG_ADDRESS  0x03F00000 // Put the debug area at the 63MB area in SDRAM
#define D64_DEBUG_ADDRESS_SIZE 1*1024*1024

#define D64_REGISTER_STATUS  0x00000200
#define D64_REGISTER_COMMAND 0x00000208
#define D64_REGISTER_LBA     0x00000210
#define D64_REGISTER_LENGTH  0x00000218
#define D64_REGISTER_RESULT  0x00000220

#define D64_REGISTER_MAGIC    0x000002EC
#define D64_REGISTER_VARIANT  0x000002F0
#define D64_REGISTER_BUTTON   0x000002F8
#define D64_REGISTER_REVISION 0x000002FC

#define D64_REGISTER_USBCOMSTAT 0x00000400
#define D64_REGISTER_USBP0R0    0x00000404
#define D64_REGISTER_USBP1R1    0x00000408

#define D64_ENABLE_ROMWR  0xF0
#define D64_DISABLE_ROMWR 0xF1
#define D64_COMMAND_WRITE 0x08

// Cartridge Interface return values
#define D64_MAGIC    0x55444556

#define D64_USB_IDLE        0x00
#define D64_USB_DATA        0x02
#define D64_USB_BUSY        0x0F
#define D64_USB_DISARM      0x0F
#define D64_USB_ARM         0x0A
#define D64_USB_ARMED       0x01
#define D64_USB_ARMING      0x0F
#define D64_USB_IDLEUNARMED 0x00

#define D64_CI_IDLE  0x00
#define D64_CI_BUSY  0x10
#define D64_CI_WRITE 0x20


/*********************************
         EverDrive macros
*********************************/

// TODO


/*********************************
             Structs
*********************************/

typedef struct {
    u32 mask;
    u32 value;
    char *string;
} regDesc;


/*********************************
        Function Prototypes
*********************************/

static void debug_handleInput();
static void debug_printRegister(u32 value, char *name, regDesc *desc);
static void debug_64drive_print(char* message);
static u8   debug_64drive_poll();
#if USE_FAULTTHREAD
    static void thread_fault(void *arg);
#endif


/*********************************
             Globals
*********************************/

static s8 debug_cart = CART_NONE;
static u8 debug_bufferout[BUFFER_SIZE];
static u8 debug_bufferin[BUFFER_SIZE];

// Message globals
OSMesg      dmaMessageBuf;
OSIoMesg    dmaIOMessageBuf;
OSMesgQueue dmaMessageQ;

// Function pointers
void (*funcPointer_print)(char*);
u8   (*funcPointer_poll)();

// Assertion globals
int assert_line = 0;
const char* assert_file = NULL;
const char* assert_expr = NULL;

// Fault thread globals
#if USE_FAULTTHREAD
    static OSMesgQueue faultMessageQ;
    static OSMesg      faultMessageBuf;
    static OSThread    faultThread;
    static u64         faultThreadStack[FAULT_THREAD_STACK/sizeof(u64)];
#endif

// List of error causes
static regDesc causeDesc[] = {
    {CAUSE_BD,      CAUSE_BD,    "BD"},
    {CAUSE_IP8,     CAUSE_IP8,   "IP8"},
    {CAUSE_IP7,     CAUSE_IP7,   "IP7"},
    {CAUSE_IP6,     CAUSE_IP6,   "IP6"},
    {CAUSE_IP5,     CAUSE_IP5,   "IP5"},
    {CAUSE_IP4,     CAUSE_IP4,   "IP4"},
    {CAUSE_IP3,     CAUSE_IP3,   "IP3"},
    {CAUSE_SW2,     CAUSE_SW2,   "IP2"},
    {CAUSE_SW1,     CAUSE_SW1,   "IP1"},
    {CAUSE_EXCMASK, EXC_INT,     "Interrupt"},
    {CAUSE_EXCMASK, EXC_MOD,     "TLB modification exception"},
    {CAUSE_EXCMASK, EXC_RMISS,   "TLB exception on load or instruction fetch"},
    {CAUSE_EXCMASK, EXC_WMISS,   "TLB exception on store"},
    {CAUSE_EXCMASK, EXC_RADE,    "Address error on load or instruction fetch"},
    {CAUSE_EXCMASK, EXC_WADE,    "Address error on store"},
    {CAUSE_EXCMASK, EXC_IBE,     "Bus error exception on instruction fetch"},
    {CAUSE_EXCMASK, EXC_DBE,     "Bus error exception on data reference"},
    {CAUSE_EXCMASK, EXC_SYSCALL, "System call exception"},
    {CAUSE_EXCMASK, EXC_BREAK,   "Breakpoint exception"},
    {CAUSE_EXCMASK, EXC_II,      "Reserved instruction exception"},
    {CAUSE_EXCMASK, EXC_CPU,     "Coprocessor unusable exception"},
    {CAUSE_EXCMASK, EXC_OV,      "Arithmetic overflow exception"},
    {CAUSE_EXCMASK, EXC_TRAP,    "Trap exception"},
    {CAUSE_EXCMASK, EXC_VCEI,    "Virtual coherency exception on intruction fetch"},
    {CAUSE_EXCMASK, EXC_FPE,     "Floating point exception (see fpcsr)"},
    {CAUSE_EXCMASK, EXC_WATCH,   "Watchpoint exception"},
    {CAUSE_EXCMASK, EXC_VCED,    "Virtual coherency exception on data reference"},
    {0,             0,           ""}
};

// List of register descriptions
static regDesc srDesc[] = {
    {SR_CU3,      SR_CU3,     "CU3"},
    {SR_CU2,      SR_CU2,     "CU2"},
    {SR_CU1,      SR_CU1,     "CU1"},
    {SR_CU0,      SR_CU0,     "CU0"},
    {SR_RP,       SR_RP,      "RP"},
    {SR_FR,       SR_FR,      "FR"},
    {SR_RE,       SR_RE,      "RE"},
    {SR_BEV,      SR_BEV,     "BEV"},
    {SR_TS,       SR_TS,      "TS"},
    {SR_SR,       SR_SR,      "SR"},
    {SR_CH,       SR_CH,      "CH"},
    {SR_CE,       SR_CE,      "CE"},
    {SR_DE,       SR_DE,      "DE"},
    {SR_IBIT8,    SR_IBIT8,   "IM8"},
    {SR_IBIT7,    SR_IBIT7,   "IM7"},
    {SR_IBIT6,    SR_IBIT6,   "IM6"},
    {SR_IBIT5,    SR_IBIT5,   "IM5"},
    {SR_IBIT4,    SR_IBIT4,   "IM4"},
    {SR_IBIT3,    SR_IBIT3,   "IM3"},
    {SR_IBIT2,    SR_IBIT2,   "IM2"},
    {SR_IBIT1,    SR_IBIT1,   "IM1"},
    {SR_KX,       SR_KX,      "KX"},
    {SR_SX,       SR_SX,      "SX"},
    {SR_UX,       SR_UX,      "UX"},
    {SR_KSU_MASK, SR_KSU_USR, "USR"},
    {SR_KSU_MASK, SR_KSU_SUP, "SUP"},
    {SR_KSU_MASK, SR_KSU_KER, "KER"},
    {SR_ERL,      SR_ERL,     "ERL"},
    {SR_EXL,      SR_EXL,     "EXL"},
    {SR_IE,       SR_IE,      "IE"},
    {0,           0,          ""}
};

// List of floating point registers descriptions
static regDesc fpcsrDesc[] = {
    {FPCSR_FS,      FPCSR_FS,    "FS"},
    {FPCSR_C,       FPCSR_C,     "C"},
    {FPCSR_CE,      FPCSR_CE,    "Unimplemented operation"},
    {FPCSR_CV,      FPCSR_CV,    "Invalid operation"},
    {FPCSR_CZ,      FPCSR_CZ,    "Division by zero"},
    {FPCSR_CO,      FPCSR_CO,    "Overflow"},
    {FPCSR_CU,      FPCSR_CU,    "Underflow"},
    {FPCSR_CI,      FPCSR_CI,    "Inexact operation"},
    {FPCSR_EV,      FPCSR_EV,    "EV"},
    {FPCSR_EZ,      FPCSR_EZ,    "EZ"},
    {FPCSR_EO,      FPCSR_EO,    "EO"},
    {FPCSR_EU,      FPCSR_EU,    "EU"},
    {FPCSR_EI,      FPCSR_EI,    "EI"},
    {FPCSR_FV,      FPCSR_FV,    "FV"},
    {FPCSR_FZ,      FPCSR_FZ,    "FZ"},
    {FPCSR_FO,      FPCSR_FO,    "FO"},
    {FPCSR_FU,      FPCSR_FU,    "FU"},
    {FPCSR_FI,      FPCSR_FI,    "FI"},
    {FPCSR_RM_MASK, FPCSR_RM_RN, "RN"},
    {FPCSR_RM_MASK, FPCSR_RM_RZ, "RZ"},
    {FPCSR_RM_MASK, FPCSR_RM_RP, "RP"},
    {FPCSR_RM_MASK, FPCSR_RM_RM, "RM"},
    {0,             0,           ""}
};


/*********************************
         Debug functions
*********************************/

/*==============================
    debug_initialize
    Check if the game is running on a 64Drive or Everdrive.
==============================*/

static void debug_initialize()
{
    u32 buff;
    u32 ret;
    volatile u32 *regs_ptr = (u32 *) 0xA8040000;
    
    // Initialize the debug related globals
    memset(debug_bufferout, 0, BUFFER_SIZE);
    memset(debug_bufferin, 0, BUFFER_SIZE);
    
    // Create the message queue
    osCreateMesgQueue(&dmaMessageQ, &dmaMessageBuf, 1);
    
    // Initialize the fault thread
    #if USE_FAULTTHREAD
        osCreateThread(&faultThread, FAULT_THREAD_ID, thread_fault, 0, (faultThreadStack+FAULT_THREAD_STACK/sizeof(u64)), FAULT_THREAD_PRI);
        osStartThread(&faultThread);
    #endif
    
    // Read the cartridge and check if we have a 64Drive.
    // TODO: add exception handling for other flashcarts
    osPiReadIo(D64_CIBASE_ADDRESS + D64_REGISTER_MAGIC, &buff);
    if (buff == D64_MAGIC)
        debug_cart = CART_64DRIVE;
        
    // ED 3.0
    osPiReadIo(0xA8040000 + sizeof(u32)*11, &ret);
        
    // ED X7
    osPiReadIo(0x1F800000 + 0x0014, &ret);
        
    // Send the string to the debug cart
    switch(debug_cart)
    {
        case CART_64DRIVE:
            funcPointer_print = debug_64drive_print;
            funcPointer_poll = debug_64drive_poll;
            break;
        case CART_EVERDRIVE3:
            // TODO
            break;
    }
}


/*==============================
    debug_printf
    Print a formatted message to the developer's command prompt
    @param A string to print
    @param variadic arguments to print as well
==============================*/

void debug_printf(char* message, ...)
{
    int i, j=0, delimcount;
    va_list args;
    int size = strlen(message);
    char buff[255] = {0};
    char isdelim = FALSE;
    char delim[8] = {0};
    
    // Get the variadic arguments
    va_start(args, message);
    
    // If the cartridge hasn't been initialized, do so.
    if (debug_cart == CART_NONE)
        debug_initialize();
        
    // If no debug cart exists, stop
    if (debug_cart == CART_NONE)
        return;

    // Build the string
    for (i=0; i<size; i++)
    {      
        // Decide if we're dealing with %% or something else
        if (message[i] == '%')
        {
            // Handle printing %%
            if ((i != 0 && message[i-1] == '%') || (i+1 != size && message[i+1] == '%'))
            {
                buff[j++] = message[i];
                isdelim = FALSE;
                continue;
            }
            
            // Handle delimiter start
            if (!isdelim)
            {
                memset(delim, 0, sizeof(delim)/sizeof(delim[0]));
                isdelim = TRUE;
                delim[0] = '%';
                delimcount = 1;
                continue;
            }
        }
      
        // If we're dealing with something else
        if (isdelim)
        {
            char numbuf[25] = {0};
            char* vastr = NULL;

            // Pick what to attach based on the character after the percentage symbol
            delim[delimcount++] = message[i];
            switch(message[i])
            {
                case 'c':
                    buff[j++] = (char)va_arg(args, int);
                    isdelim = FALSE;
                    break;
                case 's':
                    vastr = (char*)va_arg(args, char*);
                    break;
                case 'u':
                    sprintf(numbuf, delim, (unsigned int)va_arg(args, unsigned int));
                    break;
                case 'f':
                case 'e':
                    sprintf(numbuf, delim, (double)va_arg(args, double));
                    break;
                case 'x':
                case 'i':
                case 'd':
                    sprintf(numbuf, delim, (int)va_arg(args, int));
                    break;
                case 'p':
                    sprintf(numbuf, delim, (void*)va_arg(args, void*));
                    break;
                case 'l':
                case '.':
                case '0':
                case '1':
                case '2':
                case '3':
                case '4':
                case '5':
                case '6':
                case '7':
                case '8':
                case '9':
                    continue;
                default:
                    buff[j++] = '?';
                    isdelim = FALSE;
                    continue;
            }
                        
            // Append the data to the end of our string
            if (vastr != NULL)
            {
                int vastrlen = strlen(vastr);
                strcat (buff, vastr);
                j += vastrlen;
                isdelim = FALSE;
            }
            else if (numbuf[0] != '\0')
            {
                int vastrlen = strlen(numbuf);
                strcat (buff, numbuf);
                j += vastrlen;
                isdelim = FALSE;
            }
        }
        else
            buff[j++] = message[i];
    }
    va_end(args); 
        
    // Call the print function for the specific flashcart
    funcPointer_print(buff);
}


/*==============================
    debug_printRegister
    Prints info about a register
    @param The value of the register
    @param The name of the register
    @param The registry description to use
==============================*/

static void debug_printRegister(u32 value, char *name, regDesc *desc)
{
    char first = 1;

    debug_printf("%s\t\t0x%08x ", name, value);
    debug_printf("<");
    while (desc->mask != 0) 
    {
        if ((value & desc->mask) == desc->value) 
        {
            (first) ? (first = 0) : (debug_printf(","));
            debug_printf("%s", desc->string);
        }
        desc++;
    }
    debug_printf(">\n");
}


/*==============================
    _debug_assert
    Halts the program
    @param The expression that was tested
    @param The file where the exception ocurred
    @param The line number where the exception ocurred
==============================*/

void _debug_assert(const char* expression, const char* file, int line)
{
    // Set the assert data
    assert_expr = expression;
    assert_line = line;
    assert_file = file;
    
    // Intentionally cause a null pointer exception
    *((char*)(NULL)) = 0;
}


/*==============================
    debug_poll
    Polls the flashcart for data
==============================*/

void debug_poll()
{
    u8 success = 0;
    
    // If the cartridge hasn't been initialized, do so.
    if (debug_cart == CART_NONE)
        debug_initialize();
        
    // If no debug cart exists, stop
    if (debug_cart == CART_NONE)
        return;
        
    // Call the poll function
    success = funcPointer_poll();
    
    // If we got data, handle it
    if (success)
        debug_handleInput();
}


/*==============================
    debug_handleInput
    Handles the input given to us by the poll function
==============================*/

static void debug_handleInput()
{
    // Compare strings
    // Because I'm using strncmp, you can cheat by doing stuff like "pingas", it'll count as "ping"
    if (!strncmp(debug_bufferin, "ping", 4))
    {
        debug_printf("Pong!\n");
    }
    else
        debug_printf("Unknown command\n");
}


#if USE_FAULTTHREAD

    #define MSG_FAULT	0x10
    
    
    /*==============================
        thread_fault
        Handles the fault thread
        @param 
    ==============================*/
    
    static void thread_fault(void *arg)
    {
        OSMesg msg;
        OSThread *curr;

        // Create the message queue for the fault message
        osCreateMesgQueue(&faultMessageQ, &faultMessageBuf, 1);
        osSetEventMesg(OS_EVENT_FAULT, &faultMessageQ, (OSMesg)MSG_FAULT);

        // Thread loop
        while (1)
        {
            // Wait for a fault message to arrive
            osRecvMesg(&faultMessageQ, (OSMesg *)&msg, OS_MESG_BLOCK);
            
            // Get the faulted thread
            curr = (OSThread *)__osGetCurrFaultedThread();
            if (curr != NULL) 
            {
                __OSThreadContext* context = &curr->context;

                // Print the basic info
                debug_printf("Fault in thread: %d\n\n", curr->id);
                debug_printf("pc\t\t0x%08x\n", context->pc);
                if (assert_file == NULL)
                    debug_printRegister(context->cause, "cause", causeDesc);
                else
                    debug_printf("cause\t\tAssertion failed in file '%s', line %d.\n", assert_file, assert_line);
                debug_printRegister(context->sr, "sr", srDesc);
                debug_printf("badvaddr\t0x%08x\n\n", context->badvaddr);
                
                // Print the registers
                debug_printf("at 0x%016llx v0 0x%016llx v1 0x%016llx\n", context->at, context->v0, context->v1);
                debug_printf("a0 0x%016llx a1 0x%016llx a2 0x%016llx\n", context->a0, context->a1, context->a2);
                debug_printf("a3 0x%016llx t0 0x%016llx t1 0x%016llx\n", context->a3, context->t0, context->t1);
                debug_printf("t2 0x%016llx t3 0x%016llx t4 0x%016llx\n", context->t2, context->t3, context->t4);
                debug_printf("t5 0x%016llx t6 0x%016llx t7 0x%016llx\n", context->t5, context->t6, context->t7);
                debug_printf("s0 0x%016llx s1 0x%016llx s2 0x%016llx\n", context->s0, context->s1, context->s2);
                debug_printf("s3 0x%016llx s4 0x%016llx s5 0x%016llx\n", context->s3, context->s4, context->s5);
                debug_printf("s6 0x%016llx s7 0x%016llx t8 0x%016llx\n", context->s6, context->s7, context->t8);
                debug_printf("t9 0x%016llx gp 0x%016llx sp 0x%016llx\n", context->t9, context->gp, context->sp);
                debug_printf("s8 0x%016llx ra 0x%016llx\n\n",            context->s8, context->ra);

                // Print the floating point registers
                debug_printRegister(context->fpcsr, "fpcsr", fpcsrDesc);
                debug_printf("\n");
                debug_printf("d0  %.15e\td2  %.15e\n", context->fp0.d,  context->fp2.d);
                debug_printf("d4  %.15e\td6  %.15e\n", context->fp4.d,  context->fp6.d);
                debug_printf("d8  %.15e\td10 %.15e\n", context->fp8.d,  context->fp10.d);
                debug_printf("d12 %.15e\td14 %.15e\n", context->fp12.d, context->fp14.d);
                debug_printf("d16 %.15e\td18 %.15e\n", context->fp16.d, context->fp18.d);
                debug_printf("d20 %.15e\td22 %.15e\n", context->fp20.d, context->fp22.d);
                debug_printf("d24 %.15e\td26 %.15e\n", context->fp24.d, context->fp26.d);
                debug_printf("d28 %.15e\td30 %.15e\n", context->fp28.d, context->fp30.d);
            }
        }
    }
#endif


/*********************************
        64Drive functions
*********************************/

/*==============================
    debug_64drive_wait
    Wait until the 64Drive is ready
    @return 0 if success or -1 if failure
==============================*/

static s8 debug_64drive_wait()
{
    u32 ret;
    u32 timeout = 0; // I wanted to use osGetTime() but that requires the VI manager
    
    // Wait until the cartridge interface is ready
    do
    {
        osPiReadIo(D64_CIBASE_ADDRESS + D64_REGISTER_STATUS, &ret); 
        
        // Took too long, abort
        if((timeout++) > 1000000)
            return -1;
    }
    while((ret >> 8) & D64_CI_BUSY);
    
    // Success
    return 0;
}


/*==============================
    debug_64drive_setwritable
    Set the write mode on the 64Drive
    @param A boolean with whether to enable or disable
==============================*/

static void debug_64drive_setwritable(u8 enable)
{
    debug_64drive_wait();
	osPiWriteIo(D64_CIBASE_ADDRESS + D64_REGISTER_COMMAND, enable ? D64_ENABLE_ROMWR : D64_DISABLE_ROMWR); 
	debug_64drive_wait();
}


/*==============================
    debug_64drive_waitidle
    Waits for the 64Drive's USB to be idle
    @return 
==============================*/

static u32 debug_64drive_waitidle()
{
    u32 status;
    do 
    {
        osPiReadIo(D64_CIBASE_ADDRESS + D64_REGISTER_USBCOMSTAT, &status);
        status = (status >> 4) & D64_USB_BUSY;
    }
    while(status != D64_USB_IDLE);
}


/*==============================
    debug_64drive_waitdata
    Waits for the 64Drive's USB to receive data
==============================*/

static void debug_64drive_waitdata()
{
    u32 status;
    do
    {
         osPiReadIo(D64_CIBASE_ADDRESS + D64_REGISTER_USBCOMSTAT, &status); 
         status &= 0x0F;
    }
    while (status == D64_USB_IDLEUNARMED || status == D64_USB_ARMED);
}


/*==============================
    debug_64drive_waitdisarmed
    Waits for the 64Drive's USB to be disarmed
==============================*/

static void debug_64drive_waitdisarmed()
{
    u32 status;
    do
    {
         osPiReadIo(D64_CIBASE_ADDRESS + D64_REGISTER_USBCOMSTAT, &status); 
         status &= 0x0F;
    }
    while (status != D64_USB_IDLEUNARMED);
}


/*==============================
    debug_64drive_print
    Prints a message when a 64Drive is connected
    @param A string to print
==============================*/

static void debug_64drive_print(char* message)
{
    u32 i;
    u32 length = strlen(message)+1;
    
    // Don't allow messages that are too large
    if(length > BUFFER_SIZE)
        length = BUFFER_SIZE;
    
    // Copy the message to the global buffer
    memcpy(debug_bufferout, message, length);
    
    // If the message is not 32-bit aligned, pad it
    if(length%4 != 0)
    {
        u32 length_new = (length & ~3)+4;
        for(i=length; i<length_new; i++) 
            debug_bufferout[i] = 0;
        length = length_new;
    }
    
    // Spin until the write buffer is free and then set the cartridge to write mode
    debug_64drive_waitidle();
    debug_64drive_setwritable(TRUE);
    
    // Set up DMA transfer between RDRAM and the PI
    osWritebackDCacheAll();
	osPiStartDma(&dmaIOMessageBuf, OS_MESG_PRI_NORMAL, OS_WRITE, D64_BASE_ADDRESS + D64_DEBUG_ADDRESS, debug_bufferout, length, &dmaMessageQ);
	(void)osRecvMesg(&dmaMessageQ, NULL, OS_MESG_BLOCK);
    
    // Write the data to the 64Drive
	osPiWriteIo(D64_CIBASE_ADDRESS + D64_REGISTER_USBP0R0, D64_DEBUG_ADDRESS >> 1);
	osPiWriteIo(D64_CIBASE_ADDRESS + D64_REGISTER_USBP1R1, (length & 0xFFFFFF) | (DATATYPE_TEXT << 24));
	osPiWriteIo(D64_CIBASE_ADDRESS + D64_REGISTER_USBCOMSTAT, D64_COMMAND_WRITE);
    
    // Spin until the write buffer is free and then disable write mode
    debug_64drive_waitidle();
    debug_64drive_setwritable(FALSE);
}


/*==============================
    debug_64drive_poll
    Polls the 64Drive for commands to run
    @return 1 if success, 0 otherwise
==============================*/

static u8 debug_64drive_poll()
{
    char test[256];
    u32 ret;
    u32 length=0;
    u32 remaining=0;
    u8 type=0; // Unused for now, will later be used to detect what was inputted (text, file, etc...)
    
    // Don't do anything if the cart isn't initialized
    if (debug_cart == CART_NONE)
        return 0;
    
    // Check if we've received data
    osPiReadIo(D64_CIBASE_ADDRESS + D64_REGISTER_USBCOMSTAT, &ret);
    if (ret != D64_USB_DATA)
        return 0;
        
    // Check if the USB is armed, and arm it if it isn't
    osPiReadIo(D64_CIBASE_ADDRESS + D64_REGISTER_USBCOMSTAT, &ret); 
    if (ret != D64_USB_ARMING || ret != D64_USB_ARMED)
    {
        // Ensure the 64Drive is idle
        debug_64drive_waitidle();
        
        // Arm the USB FIFO DMA
        osPiWriteIo(D64_CIBASE_ADDRESS + D64_REGISTER_USBP0R0, D64_DEBUG_ADDRESS >> 1);
        osPiWriteIo(D64_CIBASE_ADDRESS + D64_REGISTER_USBP1R1, BUFFER_SIZE & 0xFFFFFF);
        osPiWriteIo(D64_CIBASE_ADDRESS + D64_REGISTER_USBCOMSTAT, D64_USB_ARM);
    }  
    
    // Read data
    do
    {
        // Wait for data
        debug_64drive_waitdata();
        
        // See how much data is left to read
        osPiReadIo(D64_CIBASE_ADDRESS + D64_REGISTER_USBP0R0, &ret); 
        length = ret & 0xFFFFFF;
        type = (ret >> 24) & 0xFF;
        osPiReadIo(D64_CIBASE_ADDRESS + D64_REGISTER_USBP1R1, &ret); 
        remaining = ret & 0xFFFFFF;
        
        // Set up DMA transfer between RDRAM and the PI
        osWritebackDCacheAll();
        osPiStartDma(&dmaIOMessageBuf, OS_MESG_PRI_NORMAL, OS_READ, D64_BASE_ADDRESS + D64_DEBUG_ADDRESS, debug_bufferin, length, &dmaMessageQ);
        (void)osRecvMesg(&dmaMessageQ, NULL, OS_MESG_BLOCK);
    }
    while (remaining > 0);
    
    // Disarm the USB and return success
    osPiWriteIo(D64_CIBASE_ADDRESS + D64_REGISTER_USBCOMSTAT, D64_USB_DISARM); 
    debug_64drive_waitdisarmed();
    return 1;
}

#else
    void debug_printf(char* message, ...){}
    void debug_poll(){}
    void _debug_assert(const char* expression, const char* file, int line){}
#endif
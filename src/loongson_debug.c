#include "loongson_debug.h"
#include "loongson_options.h"
#include "loongson_driver.h"

Bool armsocDebug;


void LS_PrepareDebug(ScrnInfoPtr pScrn)
{

    loongsonRecPtr pLS = loongsonPTR(pScrn);

    /* Determine if the user wants debug messages turned on: */
    armsocDebug = xf86ReturnOptValBool(pLS->pOptionInfo, OPTION_DEBUG, FALSE);
}

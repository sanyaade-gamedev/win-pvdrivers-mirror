!IFDEF VERSION_MAJOR
C_DEFINES=$(C_DEFINES) -DVERSION_MAJOR=$(VERSION_MAJOR)
!ENDIF
!IFDEF VERSION_MINOR
C_DEFINES=$(C_DEFINES) -DVERSION_MINOR=$(VERSION_MINOR)
!ENDIF
!IFDEF REVISION
C_DEFINES=$(C_DEFINES) -DREVISION=$(REVISION)
!ENDIF
!IFDEF BUILD_NUMBER
C_DEFINES=$(C_DEFINES) -DBUILD_NUMBER=$(BUILD_NUMBER)
!ENDIF
MSC_WARNING_LEVEL=/W4
C_DEFINES=$(C_DEFINES) -DWIN_KERNEL_BUILD
LIBLFDS_DIR = ..\liblfds.6
INCLUDES = ..\common\include;..\common\include\public;$(LIBLFDS_DIR)\inc
TARGETLIBS=$(TARGETLIBS) $(DDK_LIB_PATH)\ntstrsafe.lib
#NTTARGETFILES=

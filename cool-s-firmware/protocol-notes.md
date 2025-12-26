I: cc 18 03 00 00 bd 18 00 |........
I: 00 83 f7 46 f5 02 65 47 |...F..eG

I: 0a 25 03 00 00 fb 24 00 |.%....$.
I: 00 ab ef 5e 96 02 65 47 |...^..eG

	// Fill header
	header[0] = byte(length & 255)
	header[1] = byte((length >> 8) & 255)
	header[2] = dataTypeBytes[0]
	header[3] = dataTypeBytes[1]
	header[4] = byte(option)

func (sc *PacketBuilder) getDataType(dataType int) []byte {
	switch dataType {
	case TYPE_CAMERA, TYPE_DIY_IMAGE_UNREDO:
		return []byte{0, 0}
	case TYPE_VIDEO:
		return []byte{1, 0}
	case TYPE_IMAGE:
		return []byte{2, 0}
	case TYPE_GIF:
		return []byte{3, 0}
	case TYPE_TEXT:
		return []byte{0, 1}
	case TYPE_DIY_IMAGE:
		return []byte{5, 1}
	case TYPE_TEM:
		return []byte{4, 0}
	default:
		return []byte{0, 0}
	}
}

var deviceTypeMap = map[byte]byte{
	129: 2,  // -127 -> Type 2 (32x32)
	128: 0,  // -128 -> Type 0 (64x64)
	130: 4,  // -126 -> Type 4 (32x16)
	131: 3,  // -125 -> Type 3 (64x16)
	132: 1,  // -124 -> Type 1 (96x16)
	133: 5,  // -123 -> Type 5 (64x20)
	134: 6,  // -122 -> Type 6 (128x32)
	135: 7,  // -121 -> Type 7 (144x16)
	136: 8,  // -120 -> Type 8 (192x16)
	137: 9,  // -119 -> Type 9 (48x24)
	138: 10, // -118 -> Type 10 (64x32)
	139: 11, // -117 -> Type 11 (96x32)
	140: 12, // -116 -> Type 12 (128x32)
	141: 13, // -115 -> Type 13 (96x32)
	142: 14, // -114 -> Type 14 (160x32)
	143: 15, // -113 -> Type 15 (192x32)
	144: 16, // -112 -> Type 16 (256x32)
	145: 17, // -111 -> Type 17 (320x32)
	146: 18, // -110 -> Type 18 (384x32)
	147: 19, // -109 -> Type 19 (448x32)
}

packet 2816 App info request: 08 00 01 80 00 02 2c 00

2819 Led Matrix reply: 0b 00 01 80 85 02 2c 00 00 01 00

app: 04 00 05 80

matrix reply: 08 00 05 80 0f 15 01 05


DIY image:
    pixel data:
    I: write_led data:
    I: 0a 00 05 01 00 ff ff ff |........
    I: 07 00                   |..

    header: 0a 00 05 01
    rgb: ff ff ff
    pixel column: 07
    pixel row: 00

request device info message:
	cmd := []byte{
		8,                  // Command header
		0,                  // Reserved
		1,                  // Sub-command
		128,                // 0x80 (corresponds to -128 in signed byte)
		byte(now.Hour()),   // Current hour
		byte(now.Minute()), // Current minute
		byte(now.Second()), // Current second
		0,                  // Language (0 for default)
	}


writing a DIY image with one white pixel and pressing ok (save):
I: Advertising successfully started

I: Connected

I: Notifications enabled
I: Notifications enabled
I: Attribute FA write, handle: 0, conn: 0x200016c8
I: write_led data:
I: 08 00 01 80 01 1e 0a 00 |........
I: Attribute FA write, handle: 0, conn: 0x200016c8
I: write_led data:
I: 04 00 05 80             |....
I: Attribute FA write, handle: 0, conn: 0x200016c8
I: write_led data:
I: 05 00 04 01 01          |.....
I: Attribute FA write, handle: 0, conn: 0x200016c8
I: write_led data:
I: 0a 00 05 01 00 ff ff ff |........
I: 00 00                   |..
I: Attribute FA write, handle: 0, conn: 0x200016c8
I: write_led data:
I: 05 00 04 01 00          |.....
I: Attribute FA write, handle: 0, conn: 0x200016c8
I: write_led data:
I: 07 00 08 80 01 00 02    |.......


long packet inter frame packet?
example 1: 39 a0 45 23 3d 9c 7d 52 00 00 3b
example 2: 39 a0 45 23 3d 9c 7d 52 00 00 3b
.PHONY: build run app clean

build:
	swift build

run:
	swift run Syrinx

app:
	./Scripts/build-app.sh release

clean:
	rm -rf .build Syrinx.app
